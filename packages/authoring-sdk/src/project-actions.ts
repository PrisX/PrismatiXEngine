import type {AuthoringDiagnostic, ExtensionAction, ExtensionManifest,
  ExtensionParameter} from "./types.js";
import {nativeUiValueType} from "./ui-value.js";

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
      const actual = nativeUiValueType(value, true);
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
