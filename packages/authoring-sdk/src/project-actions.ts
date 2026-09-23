import type {AuthoringDiagnostic, ExtensionAction, ExtensionManifest,
  ExtensionParameter} from "./types.js";

type ObjectValue = Record<string, unknown>;

function object(value: unknown): ObjectValue | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as ObjectValue : undefined;
}

function objects(value: unknown): ObjectValue[] {
  return Array.isArray(value) ? value.map(object).filter((item): item is ObjectValue => item !== undefined) : [];
}

function issue(code: string, message: string, path: string, details: string): AuthoringDiagnostic {
  return {severity: "error", code, message, path, details};
}

const uuidPattern = /^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/iu;
const colorPattern = /^#[0-9a-f]{6}(?:[0-9a-f]{2})?$/iu;

function safeActionResourcePath(value: string): boolean {
  return value.length > 0 && new TextEncoder().encode(value).length <= 4096 && !value.includes("\\") &&
    !value.includes(":") && !value.includes("\0") && !value.startsWith("/") &&
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
    Object.entries(record).every(([key, item]) => key.length > 0 && key.length <= 256 &&
      boundedJson(item, depth + 1, budget));
}

function boundedJsonBytes(value: unknown): boolean {
  return boundedJson(value) && new TextEncoder().encode(JSON.stringify(value)).length <= 1024 * 1024;
}

/** The Variant type produced by the native UI document parser + UiAdapter. */
function actionValueType(value: unknown): string {
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
      (record.value === "" || uuidPattern.test(record.value) || safeActionResourcePath(record.value))
      ? "resource" : "invalid";
    if (record.type === "token") return keys.length === 2 && typeof record.value === "string" &&
      record.value.length > 0 ? "token" : "invalid";
  }
  return boundedJsonBytes(value) ? "object" : "invalid";
}

function parameterAccepts(parameter: ExtensionParameter, actual: string): boolean {
  return parameter.type === actual || parameter.type === "number" && actual === "integer";
}

/** Checks extension-declared Action invocations before a costly artifact or
 * Player build. Unknown IDs remain legal because Player/host code can register
 * project-specific actions that are not extension declarations. */
export function validateUiActionUsage(document: unknown,
  extensions: readonly ExtensionManifest[], path: string): readonly AuthoringDiagnostic[] {
  const root = object(document);
  if (root === undefined) return [];
  const diagnostics: AuthoringDiagnostic[] = [];
  const declarations = new Map<string, ExtensionAction>();
  for (const extension of extensions) for (const action of extension.actions)
    declarations.set(action.id, action);

  const check = (id: unknown, authored: unknown, location: string,
    dynamic = new Set<string>()): void => {
    if (typeof id !== "string") return;
    const action = declarations.get(id);
    if (action === undefined) return;
    const arguments_ = object(authored) ?? {};
    const parameters = new Map(action.parameters.map((parameter) => [parameter.name, parameter]));
    for (const name of dynamic) if (!parameters.has(name))
      diagnostics.push(issue("PXBUILD1061", "Unknown extension Action argument binding",
        path, `${location}: ${id}.${name}`));
    for (const [name, value] of Object.entries(arguments_)) {
      const parameter = parameters.get(name);
      if (parameter === undefined) {
        diagnostics.push(issue("PXBUILD1061", "Unknown extension Action argument",
          path, `${location}: ${id}.${name}`));
        continue;
      }
      if (dynamic.has(name)) continue;
      const actual = actionValueType(value);
      if (!parameterAccepts(parameter, actual)) {
        diagnostics.push(issue("PXBUILD1062",
          `Extension Action argument expects ${parameter.type} but received ${actual}`,
          path, `${location}: ${id}.${name}`));
        continue;
      }
      if (parameter.enum !== undefined && !parameter.enum.includes(value as never))
        diagnostics.push(issue("PXBUILD1063", "Extension Action argument is not an allowed enum value",
          path, `${location}: ${id}.${name}`));
      if (typeof value === "number" &&
          (parameter.range?.minimum !== undefined && value < parameter.range.minimum ||
           parameter.range?.maximum !== undefined && value > parameter.range.maximum))
        diagnostics.push(issue("PXBUILD1064", "Extension Action argument is outside its allowed range",
          path, `${location}: ${id}.${name}`));
    }
    for (const parameter of action.parameters) {
      if (parameter.required && parameter.default == null &&
          !Object.hasOwn(arguments_, parameter.name) && !dynamic.has(parameter.name))
        diagnostics.push(issue("PXBUILD1060", "Missing required extension Action argument",
          path, `${location}: ${id}.${parameter.name}`));
    }
  };

  for (const [index, node] of objects(root.nodes).entries()) {
    const onClick = object(node.onClick);
    if (onClick !== undefined)
      check(onClick.id, onClick.arguments, `nodes[${index}].onClick`);
    const signals = object(object(node.componentInstance)?.publicSignals) ?? {};
    for (const [signal, value] of Object.entries(signals)) {
      const binding = object(value);
      if (binding === undefined) continue;
      check(binding.id, binding.arguments,
        `nodes[${index}].componentInstance.publicSignals.${signal}`,
        new Set(Object.keys(object(binding.argumentBindings) ?? {})));
    }
  }
  const graph = object(root.behaviorGraph);
  const links = objects(graph?.links);
  for (const [index, node] of objects(graph?.nodes).entries()) {
    if (node.kind !== "action") continue;
    const dynamic = new Set(links.filter((link) => link.toNodeId === node.id &&
      typeof link.toPin === "string" && link.toPin.startsWith("arg:"))
      .map((link) => (link.toPin as string).slice(4)));
    check(object(node.properties)?.action, node.arguments,
      `behaviorGraph.nodes[${index}]`, dynamic);
  }
  return diagnostics;
}
