import type {UiTypeRegistryMetadata} from "./semantic.js";
import type {AuthoringDiagnostic, JsonValue, SourceSpan} from "./types.js";
import type {ProjectDocument} from "./project.js";
import {jsonPointerSpans} from "./json-source-spans.js";

type JsonObject = Record<string, JsonValue>;
type ResolvedValue = {readonly declared: boolean; readonly value: JsonValue | undefined};
type UiValueType = "null" | "boolean" | "integer" | "number" | "string" | "object" | "array";
interface UiContract {
  readonly roots: readonly string[];
  readonly values: ReadonlyMap<string, UiValueType>;
  readonly formatters: ReadonlyMap<string, readonly [UiValueType, UiValueType]>;
}

const builtinFormatters: Readonly<Record<string, readonly [string, string]>> = {
  BoolToVisibility: ["boolean", "string"],
  Not: ["boolean", "boolean"],
  Percent: ["number", "string"],
};
const valueTypes = new Set<string>(["null", "boolean", "integer", "number", "string", "object", "array"]);
const contractPathPattern = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*|\[\])*$/u;
const contractRootPattern = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/u;

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

function parseUiContract(value: JsonValue | undefined,
  locate?: (pointer: string) => SourceSpan | undefined): {
  readonly contract?: UiContract;
  readonly diagnostics: readonly AuthoringDiagnostic[];
} {
  if (value === undefined) return {diagnostics: []};
  const path = "project.pxproject";
  const at = (message: string, details: string, pointer: string): AuthoringDiagnostic => {
    const span = locate?.(pointer);
    return {...issue("PXBUILD1090", message, path, details),
      ...(span === undefined ? {} : {span})};
  };
  const escape = (name: string) => name.replaceAll("~", "~0").replaceAll("/", "~1");
  const prefix = "/settings/authoring.uiContract";
  const root = object(value);
  const roots = root?.roots;
  const values = object(root?.values);
  const formatters = object(root?.formatters);
  if (root === undefined || root.format !== "PrismatiXUiContract" ||
    root.schemaRevision !== 1 || Object.keys(root).some((key) =>
      key !== "format" && key !== "schemaRevision" &&
      key !== "roots" && key !== "values" && key !== "formatters") ||
    !Array.isArray(roots) || roots.length > 256 ||
    !roots.every((name) => typeof name === "string" && name.length <= 512 &&
      contractRootPattern.test(name)) || new Set(roots).size !== roots.length ||
    (values === undefined && formatters === undefined) ||
    (values !== undefined && Object.keys(values).length > 1024) ||
    (formatters !== undefined && Object.keys(formatters).length > 128))
    return {diagnostics: [at("authoring.uiContract has an invalid envelope",
      "settings.authoring.uiContract", prefix)]};
  const declaredRoots = roots as string[];
  const declaredValues = new Map<string, UiValueType>();
  for (const [name, type] of Object.entries(values ?? {})) {
    if (name.length > 512 || !contractPathPattern.test(name) ||
        typeof type !== "string" || !valueTypes.has(type) ||
        !declaredRoots.some((declaredRoot) => name === declaredRoot ||
          name.startsWith(`${declaredRoot}.`) || name.startsWith(`${declaredRoot}[`)))
      return {diagnostics: [at("authoring.uiContract value declaration is invalid",
        `settings.authoring.uiContract.values.${name}`,
        `${prefix}/values/${escape(name)}`)]};
    declaredValues.set(name, type as UiValueType);
  }
  const missingRoot = declaredRoots.findIndex((name) => !declaredValues.has(name));
  if (missingRoot >= 0)
    return {diagnostics: [at("authoring.uiContract root needs a value type",
      "settings.authoring.uiContract.roots", `${prefix}/roots/${missingRoot}`)]};
  const declaredFormatters = new Map<string, readonly [UiValueType, UiValueType]>();
  for (const [name, raw] of Object.entries(formatters ?? {})) {
    const descriptor = object(raw);
    if (!/^[A-Za-z_][A-Za-z0-9_.-]{0,127}$/u.test(name) ||
        Object.hasOwn(builtinFormatters, name) || descriptor === undefined ||
        Object.keys(descriptor).length !== 2 ||
        typeof descriptor.input !== "string" || !valueTypes.has(descriptor.input) ||
        typeof descriptor.output !== "string" || !valueTypes.has(descriptor.output))
      return {diagnostics: [at("authoring.uiContract formatter declaration is invalid",
        `settings.authoring.uiContract.formatters.${name}`,
        `${prefix}/formatters/${escape(name)}`)]};
    declaredFormatters.set(name, [descriptor.input as UiValueType,
      descriptor.output as UiValueType]);
  }
  return {contract: {roots: declaredRoots, values: declaredValues,
    formatters: declaredFormatters}, diagnostics: []};
}

function declaredContractType(contract: UiContract | undefined, path: string): UiValueType | undefined {
  return contract?.values.get(path.replace(/\[(?:0|[1-9][0-9]*)\]/gu, "[]"));
}

function contractOwnsPath(contract: UiContract | undefined, path: string): boolean {
  if (contract === undefined) return false;
  return contract.roots.some((root) =>
    path === root || path.startsWith(`${root}.`) || path.startsWith(`${root}[`));
}

/** Validate initial Player ViewModel data and explicitly declared host data.
 * Undeclared runtime roots remain the host's responsibility. */
export function validatePlayerUiBindings(project: ProjectDocument,
  uiDocuments: ReadonlyMap<string, JsonValue>,
  registry?: UiTypeRegistryMetadata,
  documentSources?: Readonly<Record<string, string>>,
  projectSource?: {readonly path: string; readonly text: string}): readonly AuthoringDiagnostic[] {
  const data = object(project.settings?.["player.uiData"]);
  const contractValue = project.settings?.["authoring.uiContract"];
  const source = projectSource ?? (documentSources?.["project.pxproject"] === undefined
    ? undefined : {path: "project.pxproject", text: documentSources["project.pxproject"]});
  let contractSpans: ReadonlyMap<string, SourceSpan> | undefined;
  const contractSpan = (pointer: string): SourceSpan | undefined => {
    if (source === undefined) return undefined;
    if (contractSpans === undefined) {
      let authored: unknown;
      try { authored = JSON.parse(source.text) as unknown; }
      catch { return undefined; }
      const authoredContract = object(object(authored)?.settings)?.["authoring.uiContract"];
      if (JSON.stringify(authoredContract) !== JSON.stringify(contractValue)) return undefined;
      contractSpans = jsonPointerSpans(source.text, authored, source.path);
    }
    return contractSpans.get(pointer);
  };
  const parsedContract = parseUiContract(contractValue, contractSpan);
  const contract = parsedContract.contract;
  const diagnostics: AuthoringDiagnostic[] = [...parsedContract.diagnostics];
  const finish = (): readonly AuthoringDiagnostic[] => {
    if (documentSources === undefined) return diagnostics;
    const indexed = new Map<string, ReturnType<typeof jsonPointerSpans>>();
    return diagnostics.map((diagnostic) => {
      if (diagnostic.path === undefined || diagnostic.details === undefined ||
          !uiDocuments.has(diagnostic.path)) return diagnostic;
      const field = /^nodes\[(\d+)\]\.(itemSource|bindings)\.([^:]+):/u.exec(diagnostic.details);
      if (field === null) return diagnostic;
      const path = diagnostic.path;
      let spans = indexed.get(path);
      if (spans === undefined) {
        spans = jsonPointerSpans(documentSources[path], uiDocuments.get(path), path);
        indexed.set(path, spans);
      }
      const escape = (value: string) => value.replaceAll("~", "~0").replaceAll("/", "~1");
      const prefix = `/nodes/${field[1]}/${field[2]}`;
      const property = field[2] === "itemSource"
        ? field[3]!.split(".")[0]! : field[3]!;
      const target = `${prefix}/${escape(property)}`;
      const leaf = field[2] === "bindings"
        ? diagnostic.code === "PXBUILD1083" || diagnostic.code === "PXBUILD1093"
          ? "formatter" : "path"
        : undefined;
      const span = leaf === undefined ? spans.get(target) :
        spans.get(`${target}/${leaf}`) ?? spans.get(target);
      return span === undefined ? diagnostic : {...diagnostic, span};
    });
  };
  if (diagnostics.length > 0 || data === undefined && contract === undefined) return finish();
  const properties = new Map(registry?.controls.map((control) => [control.runtimeType,
    new Map(control.properties.map((property) => [property.id, property.valueType]))]) ?? []);
  const checkType = (sourceType: string, formatter: unknown, runtimeType: unknown,
    target: string, path: string, details: string): void => {
    const knownFormatter = typeof formatter === "string"
      ? builtinFormatters[formatter] ?? contract?.formatters.get(formatter) : undefined;
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
        const resolved = data === undefined ? {declared: false, value: undefined}
          : resolveDeclared(data, ownSource.path);
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
            if (diagnostics.length >= 64) return finish();
          }
        }
        if (contractOwnsPath(contract, ownSource.path)) {
          const type = declaredContractType(contract, ownSource.path);
          if (type !== "array")
            diagnostics.push(issue("PXBUILD1091", "UI itemSource must be declared as an array",
              path, `${location}.itemSource.path: ${ownSource.path}`));
          else {
            const itemPrefix = `${ownSource.path}[]`;
            const keyType = typeof ownSource.key === "string"
              ? declaredContractType(contract, `${itemPrefix}.${ownSource.key}`) : undefined;
            if (keyType !== "string" && keyType !== "integer")
              diagnostics.push(issue("PXBUILD1092", "UI item key contract must be string or integer",
                path, `${location}.itemSource.key: ${String(ownSource.key)}`));
            if (typeof ownSource.placement === "string") for (const field of
              ["x", "y", "width", "height"]) {
              const fieldType = declaredContractType(contract,
                `${itemPrefix}.${ownSource.placement}.${field}`);
              if (fieldType !== "number" && fieldType !== "integer")
                diagnostics.push(issue("PXBUILD1092", "UI item placement contract needs numeric fields",
                  path, `${location}.itemSource.placement.${field}`));
            }
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
        if (typeof binding.formatter === "string" &&
            !Object.hasOwn(builtinFormatters, binding.formatter) &&
            contract !== undefined && !contract.formatters.has(binding.formatter))
          diagnostics.push(issue("PXBUILD1093", "UI custom formatter is not declared in authoring.uiContract",
            path, detail));
        if (source !== undefined && binding.path.startsWith("item.") &&
            typeof source.path === "string") {
          const resolved = data === undefined ? {declared: false, value: undefined}
            : resolveDeclared(data, source.path);
          if (Array.isArray(resolved.value)) {
            for (const [itemIndex, item] of resolved.value.entries()) {
              const field = itemField(item, binding.path.slice(5));
              if (field === undefined)
                diagnostics.push(issue("PXBUILD1085", "UI item binding field is missing in player.uiData",
                  path, `${detail}, item[${itemIndex}]`));
              else checkType(variantType(field), binding.formatter, node.runtimeType, target,
                path, `${detail}, item[${itemIndex}]`);
              if (diagnostics.length >= 64) return finish();
            }
          }
          if (declaredContractType(contract, source.path) === "array") {
            const type = declaredContractType(contract,
              `${source.path}[].${binding.path.slice(5)}`);
            if (type === undefined)
              diagnostics.push(issue("PXBUILD1094", "UI item binding field is missing in authoring.uiContract",
                path, detail));
            else checkType(type, binding.formatter, node.runtimeType, target, path, detail);
          }
        } else if (source === undefined) {
          const resolved = data === undefined ? {declared: false, value: undefined}
            : resolveDeclared(data, binding.path);
          const declaredType = declaredContractType(contract, binding.path);
          if (resolved.declared && resolved.value === undefined && declaredType === undefined)
            diagnostics.push(issue("PXBUILD1086", "UI ViewModel path is missing in player.uiData",
              path, detail));
          else if (resolved.value !== undefined) {
            if (declaredType !== undefined && declaredType !== variantType(resolved.value))
              diagnostics.push(issue("PXBUILD1095", "UI ViewModel value contradicts authoring.uiContract",
                path, detail));
            checkType(variantType(resolved.value), binding.formatter,
              node.runtimeType, target, path, detail);
          }
          else if (contractOwnsPath(contract, binding.path)) {
            if (declaredType === undefined)
              diagnostics.push(issue("PXBUILD1094", "UI ViewModel path is missing in authoring.uiContract",
                path, detail));
            else checkType(declaredType, binding.formatter, node.runtimeType, target, path, detail);
          }
        }
        if (diagnostics.length >= 64) return finish();
      }
    }
  }
  return finish();
}
