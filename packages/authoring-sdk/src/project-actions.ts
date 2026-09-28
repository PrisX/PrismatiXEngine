import type {AuthoringDiagnostic, ExtensionAction, ExtensionManifest,
  ExtensionSafety,
  ExtensionParameter} from "./types.js";
import {nativeUiValueType} from "./ui-value.js";
import {jsonPointerSpans} from "./json-source-spans.js";
import {validateSemantics} from "./semantic.js";

type ObjectValue = Record<string, unknown>;

function object(value: unknown): ObjectValue | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as ObjectValue : undefined;
}

function objects(value: unknown): ObjectValue[] {
  return Array.isArray(value) ? value.map(object).filter((item): item is ObjectValue => item !== undefined) : [];
}

function indexedObjects(value: unknown): Array<readonly [number, ObjectValue]> {
  return Array.isArray(value) ? value.flatMap((item, index) => {
    const entry = object(item);
    return entry === undefined ? [] : [[index, entry] as const];
  }) : [];
}

function issue(code: string, message: string, path: string, details: string): AuthoringDiagnostic {
  return {severity: "error", code, message, path, details};
}

function parameterAccepts(parameter: ExtensionParameter, actual: string): boolean {
  return parameter.type === actual || parameter.type === "number" && actual === "integer";
}

export interface HostActionParameter {
  readonly name: string;
  readonly type: "boolean" | "integer" | "number" | "string" | "resource";
  readonly required?: boolean;
}

export interface HostActionDeclaration {
  readonly id: string;
  readonly parameters: readonly HostActionParameter[];
  readonly safety: ExtensionSafety;
}

export interface HostActionContract {
  readonly format: "PrismatiXActionContract";
  readonly schemaRevision: 1;
  readonly actions: readonly HostActionDeclaration[];
}

/** Project schema checks the envelope; extension Action semantics check typed
 * parameters and safety with the same rules used for scripted Actions. */
export function validateHostActionContract(value: HostActionContract,
  path: string): readonly AuthoringDiagnostic[] {
  return validateSemantics("extension", {actions: value.actions, commands: [],
    capabilities: []}, path);
}

/** Checks declared Action invocations before a costly artifact or Player build.
 * Unknown IDs remain legal because Player/host code can register additional
 * project-specific Actions. */
export function validateUiActionUsage(document: unknown,
  extensions: readonly ExtensionManifest[], path: string,
  source?: string, hostActions: readonly ExtensionAction[] = []): readonly AuthoringDiagnostic[] {
  const root = object(document);
  if (root === undefined) return [];
  const diagnostics: AuthoringDiagnostic[] = [];
  let sourceSpans: ReturnType<typeof jsonPointerSpans> | undefined;
  const report = (code: string, message: string, details: string,
    pointer: string, fallback: string): void => {
    if (source !== undefined)
      sourceSpans ??= jsonPointerSpans(source, document, path);
    const span = sourceSpans?.get(pointer) ?? sourceSpans?.get(fallback);
    diagnostics.push({...issue(code, message, path, details),
      ...(span === undefined ? {} : {span})});
  };
  const escapePointer = (part: string) => part.replaceAll("~", "~0").replaceAll("/", "~1");
  const declarations = new Map<string, ExtensionAction>();
  for (const extension of extensions) for (const action of extension.actions)
    declarations.set(action.id, action);
  for (const action of hostActions) declarations.set(action.id, action);
  const nodes = objects(root.nodes);
  const nodeById = new Map(nodes.filter((node) => typeof node.id === "string")
    .map((node) => [node.id as string, node]));
  const inItemTemplate = (node: ObjectValue): boolean => {
    const visited = new Set<string>();
    let parent = typeof node.parentId === "string" ? node.parentId : undefined;
    while (parent !== undefined && !visited.has(parent)) {
      visited.add(parent);
      const ancestor = nodeById.get(parent);
      if (object(ancestor?.itemSource) !== undefined) return true;
      parent = typeof ancestor?.parentId === "string" ? ancestor.parentId : undefined;
    }
    return false;
  };

  const check = (id: unknown, authored: unknown, location: string,
    pointer: string, idPointer: string,
    dynamic = new Set<string>(), itemTemplate = false): void => {
    if (typeof id !== "string") return;
    const action = declarations.get(id);
    if (action === undefined) return;
    const arguments_ = object(authored) ?? {};
    const parameters = new Map(action.parameters.map((parameter) => [parameter.name, parameter]));
    for (const name of dynamic) if (!parameters.has(name))
      report("PXBUILD1061", "Unknown Action argument binding",
        `${location}: ${id}.${name}`,
        `${pointer}/argumentBindings/${escapePointer(name)}`, idPointer);
    for (const [name, value] of Object.entries(arguments_)) {
      const parameter = parameters.get(name);
      const argumentPointer = `${pointer}/arguments/${escapePointer(name)}`;
      if (parameter === undefined) {
        report("PXBUILD1061", "Unknown Action argument",
          `${location}: ${id}.${name}`, argumentPointer, idPointer);
        continue;
      }
      if (dynamic.has(name)) continue;
      if (itemTemplate && (value === "$index" || value === "$key")) {
        const accepted = value === "$index"
          ? parameterAccepts(parameter, "integer")
          : ["string", "integer", "number"].includes(parameter.type);
        if (!accepted) report("PXBUILD1062",
          `Action argument expects ${parameter.type} but received item ${value}`,
          `${location}: ${id}.${name}`, argumentPointer, idPointer);
        continue;
      }
      const actual = nativeUiValueType(value, true);
      if (!parameterAccepts(parameter, actual)) {
        report("PXBUILD1062",
          `Action argument expects ${parameter.type} but received ${actual}`,
          `${location}: ${id}.${name}`, argumentPointer, idPointer);
        continue;
      }
      if (parameter.enum !== undefined && !parameter.enum.includes(value as never))
        report("PXBUILD1063", "Action argument is not an allowed enum value",
          `${location}: ${id}.${name}`, argumentPointer, idPointer);
      if (typeof value === "number" &&
          (parameter.range?.minimum !== undefined && value < parameter.range.minimum ||
           parameter.range?.maximum !== undefined && value > parameter.range.maximum))
        report("PXBUILD1064", "Action argument is outside its allowed range",
          `${location}: ${id}.${name}`, argumentPointer, idPointer);
    }
    for (const parameter of action.parameters) {
      if (parameter.required && parameter.default == null &&
          !Object.hasOwn(arguments_, parameter.name) && !dynamic.has(parameter.name))
        report("PXBUILD1060", "Missing required Action argument",
          `${location}: ${id}.${parameter.name}`, idPointer, pointer);
    }
  };

  for (const [index, node] of indexedObjects(root.nodes)) {
    const onClick = object(node.onClick);
    if (onClick !== undefined)
      check(onClick.id, onClick.arguments, `nodes[${index}].onClick`,
        `/nodes/${index}/onClick`, `/nodes/${index}/onClick/id`,
        new Set<string>(), inItemTemplate(node));
    const signals = object(object(node.componentInstance)?.publicSignals) ?? {};
    for (const [signal, value] of Object.entries(signals)) {
      const binding = object(value);
      if (binding === undefined) continue;
      check(binding.id, binding.arguments,
        `nodes[${index}].componentInstance.publicSignals.${signal}`,
        `/nodes/${index}/componentInstance/publicSignals/${escapePointer(signal)}`,
        `/nodes/${index}/componentInstance/publicSignals/${escapePointer(signal)}/id`,
        new Set(Object.keys(object(binding.argumentBindings) ?? {})),
        inItemTemplate(node));
    }
  }
  const graph = object(root.behaviorGraph);
  const links = objects(graph?.links);
  for (const [index, node] of indexedObjects(graph?.nodes)) {
    if (node.kind !== "action") continue;
    const dynamic = new Set(links.filter((link) => link.toNodeId === node.id &&
      typeof link.toPin === "string" && link.toPin.startsWith("arg:"))
      .map((link) => (link.toPin as string).slice(4)));
    check(object(node.properties)?.action, node.arguments,
      `behaviorGraph.nodes[${index}]`,
      `/behaviorGraph/nodes/${index}`,
      `/behaviorGraph/nodes/${index}/properties/action`, dynamic);
  }
  return diagnostics;
}
