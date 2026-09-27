import type {UiTypeRegistryMetadata} from "./semantic.js";
import type {AuthoringDiagnostic, JsonValue} from "./types.js";
import type {ProjectDocument} from "./project.js";

type JsonObject = Record<string, JsonValue>;
type ResolvedValue = {readonly declared: boolean; readonly value: JsonValue | undefined};

const builtinFormatters: Readonly<Record<string, readonly [string, string]>> = {
  BoolToVisibility: ["boolean", "string"],
  Not: ["boolean", "boolean"],
  Percent: ["number", "string"],
};

function object(value: unknown): JsonObject | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as JsonObject : undefined;
}

function variantType(value: JsonValue): string {
  if (value === null) return "null";
  if (Array.isArray(value)) return "array";
  if (typeof value === "object") return "object";
  if (typeof value === "number") return Number.isInteger(value) ? "integer" : "number";
  return typeof value;
}

// Player defines each player.uiData key as one ViewModel root. Nested paths
// follow ObservableViewModel::ResolvePath and the indexed property grammar.
function resolveDeclared(data: JsonObject, path: string): ResolvedValue {
  if (Object.hasOwn(data, path)) return {declared: true, value: data[path]};
  const roots = Object.keys(data).filter((candidate) => path.startsWith(candidate) &&
    (path[candidate.length] === "." || path[candidate.length] === "["))
    .sort((a, b) => b.length - a.length);
  if (roots.length === 0) return {declared: false, value: undefined};
  // Native keeps the last successfully resolved root when a longer root is
  // present but does not contain the requested nested field.
  for (const root of roots) {
    let value: JsonValue | undefined = data[root];
    let position = root.length;
    while (position < path.length) {
      const tail = path.slice(position);
      const field = /^\.([A-Za-z_][A-Za-z0-9_]*)/u.exec(tail);
      if (field !== null) {
        value = object(value)?.[field[1]!];
        position += field[0].length;
        continue;
      }
      const indexed = /^\[(0|[1-9][0-9]*)\]/u.exec(tail);
      if (indexed !== null) {
        value = Array.isArray(value) ? value[Number(indexed[1])] : undefined;
        position += indexed[0].length;
        continue;
      }
      value = undefined;
      break;
    }
    if (value !== undefined) return {declared: true, value};
  }
  return {declared: true, value: undefined};
}

function itemField(item: JsonValue, path: string): JsonValue | undefined {
  let value: JsonValue | undefined = item;
  for (const field of path.split(".")) value = object(value)?.[field];
  return value;
}

function validPlacement(item: JsonValue, path: string): boolean {
  const value = object(itemField(item, path));
  if (value === undefined || Object.keys(value).length !== 4) return false;
  return ["x", "y", "width", "height"].every((field) =>
    typeof value[field] === "number" && Number.isFinite(value[field]) &&
    Math.abs(value[field]) <= 1_000_000) &&
    (value.width as number) > 0 && (value.height as number) > 0;
}

function issue(code: string, message: string, path: string,
  details: string): AuthoringDiagnostic {
  return {severity: "error", code, message, path, details};
}

/** Validate the initial Player ViewModel values whose roots are explicitly
 * declared in project.settings["player.uiData"]. Runtime-defined roots remain
 * the host's responsibility; they are never guessed from UI path spelling. */
export function validatePlayerUiBindings(project: ProjectDocument,
  uiDocuments: ReadonlyMap<string, JsonValue>,
  registry?: UiTypeRegistryMetadata): readonly AuthoringDiagnostic[] {
  const data = object(project.settings?.["player.uiData"]);
  if (data === undefined) return [];
  const diagnostics: AuthoringDiagnostic[] = [];
  const properties = new Map(registry?.controls.map((control) => [control.runtimeType,
    new Map(control.properties.map((property) => [property.id, property.valueType]))]) ?? []);
  const checkType = (source: JsonValue, formatter: unknown, runtimeType: unknown,
    target: string, path: string, details: string): void => {
    const sourceType = variantType(source);
    const knownFormatter = typeof formatter === "string" ? builtinFormatters[formatter] : undefined;
    if (knownFormatter !== undefined && knownFormatter[0] !== sourceType) {
      diagnostics.push(issue("PXBUILD1083", `UI formatter ${formatter} expects ${knownFormatter[0]} but source is ${sourceType}`,
        path, details));
      return;
    }
    const outputType = knownFormatter?.[1] ?? (formatter === undefined ? sourceType : undefined);
    const targetType = typeof runtimeType === "string"
      ? properties.get(runtimeType)?.get(target) : undefined;
    if (outputType !== undefined && targetType !== undefined && outputType !== targetType)
      diagnostics.push(issue("PXBUILD1084", `UI binding produces ${outputType} but target expects ${targetType}`,
        path, details));
  };

  for (const [path, document] of uiDocuments) {
    const nodes = Array.isArray(object(document)?.nodes) ?
      object(document)!.nodes as JsonValue[] : [];
    const byId = new Map(nodes.map((value) => {
      const node = object(value);
      return [node?.id, node] as const;
    }));
    const templateSource = (node: JsonObject): JsonObject | undefined => {
      const seen = new Set<string>();
      let parent = node.parentId;
      while (typeof parent === "string" && !seen.has(parent)) {
        seen.add(parent);
        const ancestor = byId.get(parent);
        const source = object(ancestor?.itemSource);
        if (source !== undefined) return source;
        parent = ancestor?.parentId;
      }
      return undefined;
    };
    for (const [index, value] of nodes.entries()) {
      const node = object(value);
      if (node === undefined) continue;
      const location = `nodes[${index}]`;
      const ownSource = object(node.itemSource);
      if (ownSource !== undefined && typeof ownSource.path === "string") {
        const resolved = resolveDeclared(data, ownSource.path);
        if (resolved.declared && !Array.isArray(resolved.value))
          diagnostics.push(issue("PXBUILD1080", "UI itemSource does not resolve to an array in player.uiData",
            path, `${location}.itemSource.path: ${ownSource.path}`));
        if (Array.isArray(resolved.value)) {
          const keys = new Set<string>();
          for (const [itemIndex, item] of resolved.value.entries()) {
            const key = typeof ownSource.key === "string" ? itemField(item, ownSource.key) : undefined;
            const stable = typeof key === "string" && key.length > 0 ? `s:${key}`
              : typeof key === "number" && Number.isSafeInteger(key) ? `i:${key}` : undefined;
            if (stable === undefined || keys.has(stable))
              diagnostics.push(issue("PXBUILD1081", "UI item stable key is missing, invalid, or duplicated",
                path, `${location}.itemSource.key: item[${itemIndex}]`));
            else keys.add(stable);
            if (typeof ownSource.placement === "string" &&
                !validPlacement(item, ownSource.placement))
              diagnostics.push(issue("PXBUILD1082", "UI item placement is invalid",
                path, `${location}.itemSource.placement: item[${itemIndex}]`));
            if (diagnostics.length >= 64) return diagnostics;
          }
        }
      }
      const bindings = object(node.bindings);
      if (bindings === undefined) continue;
      const source = templateSource(node);
      for (const [target, rawBinding] of Object.entries(bindings)) {
        const binding = object(rawBinding);
        if (binding === undefined || typeof binding.path !== "string") continue;
        const detail = `${location}.bindings.${target}: ${binding.path}`;
        if (source !== undefined && binding.path.startsWith("item.") &&
            typeof source.path === "string") {
          const resolved = resolveDeclared(data, source.path);
          if (!Array.isArray(resolved.value)) continue;
          for (const [itemIndex, item] of resolved.value.entries()) {
            const field = itemField(item, binding.path.slice(5));
            if (field === undefined)
              diagnostics.push(issue("PXBUILD1085", "UI item binding field is missing in player.uiData",
                path, `${detail}, item[${itemIndex}]`));
            else checkType(field, binding.formatter, node.runtimeType, target,
              path, `${detail}, item[${itemIndex}]`);
            if (diagnostics.length >= 64) return diagnostics;
          }
        } else if (source === undefined) {
          const resolved = resolveDeclared(data, binding.path);
          if (!resolved.declared) continue;
          if (resolved.value === undefined)
            diagnostics.push(issue("PXBUILD1086", "UI ViewModel path is missing in player.uiData",
              path, detail));
          else checkType(resolved.value, binding.formatter, node.runtimeType, target,
            path, detail);
        }
        if (diagnostics.length >= 64) return diagnostics;
      }
    }
  }
  return diagnostics;
}
