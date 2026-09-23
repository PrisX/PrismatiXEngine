/** Matches the UiValue parser and bounded JSON fallback in native Ui.cpp.
 * Action arguments alone may additionally use a project-relative resource path. */
type ObjectValue = Record<string, unknown>;

function object(value: unknown): ObjectValue | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as ObjectValue : undefined;
}

const uuidPattern = /^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/iu;
const colorPattern = /^#[0-9a-f]{6}(?:[0-9a-f]{2})?$/iu;
const encoder = new TextEncoder();

function safeActionResourcePath(value: string): boolean {
  return value.length > 0 && encoder.encode(value).length <= 4096 &&
    !value.includes("\\") && !value.includes(":") && !value.includes("\0") &&
    !value.startsWith("/") &&
    value.split("/").every((segment) => segment !== "" && segment !== "." && segment !== "..");
}

function boundedJson(value: unknown, depth = 0, budget = {nodes: 0}): boolean {
  if (depth > 32 || ++budget.nodes > 8192) return false;
  if (value === null || typeof value === "boolean" || typeof value === "string") return true;
  if (typeof value === "number") return Number.isFinite(value);
  if (Array.isArray(value)) return value.length <= 1024 &&
    value.every((item) => boundedJson(item, depth + 1, budget));
  const record = object(value);
  return record !== undefined && Object.keys(record).length <= 256 &&
    Object.entries(record).every(([key, item]) => key.length > 0 &&
      encoder.encode(key).length <= 256 && boundedJson(item, depth + 1, budget));
}

function boundedJsonBytes(value: unknown): boolean {
  return boundedJson(value) &&
    encoder.encode(JSON.stringify(value)).length <= 1024 * 1024;
}

export function nativeUiValueType(value: unknown, allowResourcePath = false): string {
  if (value === null) return "null";
  if (typeof value === "boolean") return "boolean";
  if (typeof value === "number") return !Number.isFinite(value) ? "invalid"
    : Number.isSafeInteger(value) ? "integer" : "number";
  if (typeof value === "string") return "string";
  if (Array.isArray(value)) return boundedJsonBytes(value) ? "array" : "invalid";
  const record = object(value);
  if (record === undefined) return "invalid";
  const keys = Object.keys(record);
  const finite = (key: string) => typeof record[key] === "number" && Number.isFinite(record[key]);
  if (typeof record.type === "string") {
    if (record.type === "vec2") return keys.length === 3 && finite("x") && finite("y") ? "vec2" : "invalid";
    if (record.type === "rect") return keys.length === 5 && finite("x") && finite("y") &&
      finite("width") && finite("height") ? "rect" : "invalid";
    if (record.type === "color") return keys.length === 2 && typeof record.value === "string" &&
      colorPattern.test(record.value) ? "color" : "invalid";
    if (record.type === "uuid") return keys.length === 2 && typeof record.value === "string" &&
      uuidPattern.test(record.value) ? "uuid" : "invalid";
    if (record.type === "nodeReference") return keys.length === 2 && typeof record.nodeId === "string" &&
      uuidPattern.test(record.nodeId) ? "uuid" : "invalid";
    if (record.type === "resource") return keys.length === 2 && typeof record.value === "string" &&
      (record.value === "" || uuidPattern.test(record.value) ||
        allowResourcePath && safeActionResourcePath(record.value)) ? "resource" : "invalid";
    if (record.type === "token") return keys.length === 2 && typeof record.value === "string" &&
      record.value.length > 0 ? "token" : "invalid";
  }
  return boundedJsonBytes(value) ? "object" : "invalid";
}
