import type {AuthoringDiagnostic} from "./types.js";

type ObjectValue = Record<string, unknown>;

function object(value: unknown): ObjectValue | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value) ? value as ObjectValue : undefined;
}

function objects(value: unknown): ObjectValue[] {
  return Array.isArray(value) ? value.map(object).filter((item): item is ObjectValue => item !== undefined) : [];
}

function issue(code: string, message: string, path?: string, details?: string): AuthoringDiagnostic {
  return {severity:"error",code,message,...(path === undefined ? {} : {path}),...(details === undefined ? {} : {details})};
}

function duplicates(values: readonly unknown[]): string[] {
  const found = new Set<string>();
  const duplicate = new Set<string>();
  for (const value of values) {
    if (typeof value !== "string") continue;
    if (!found.add(value)) duplicate.add(value);
  }
  return [...duplicate].sort();
}

function uniqueIdentities(items: readonly ObjectValue[], field: string, contract: string, path?: string): AuthoringDiagnostic[] {
  return duplicates(items.map((item) => item[field])).map((id) => issue("PXSDKSEM1001", `${contract} ${field} must be unique`, path, id));
}

function variableDefaultMatches(variable: ObjectValue): boolean {
  const value = variable.default;
  switch (variable.type) {
    case "boolean": return typeof value === "boolean";
    case "integer": return typeof value === "number" && Number.isInteger(value);
    case "number": return typeof value === "number" && Number.isFinite(value);
    case "string": return typeof value === "string";
    default: return false;
  }
}

function hasCompleteSafety(value: unknown): boolean {
  const safety = object(value);
  return safety !== undefined
    && typeof safety.previewSafe === "boolean"
    && typeof safety.deterministic === "boolean"
    && typeof safety.seekSafe === "boolean"
    && typeof safety.rollbackSafe === "boolean";
}

function timelineSemantics(root: ObjectValue, path?: string): AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  const duration = typeof root.duration === "number" ? root.duration : 0;
  const tracks = objects(root.tracks);
  diagnostics.push(...uniqueIdentities(tracks, "id", "Timeline track", path));
  for (const track of tracks) {
    const keys = objects(track.keyframes ?? track.keys);
    let previous = -Infinity;
    for (const key of keys) {
      const time = typeof key.time === "number" ? key.time : -1;
      if (time < previous || time > duration) diagnostics.push(issue("PXSDKSEM1201", "Timeline keyframes must be ordered and inside the clip duration", path, String(track.id ?? "")));
      previous = time;
    }
  }
  for (const marker of objects(root.markers)) {
    if (typeof marker.time === "number" && marker.time > duration) diagnostics.push(issue("PXSDKSEM1202", "Timeline marker exceeds the clip duration", path, String(marker.id ?? marker.name ?? "")));
  }
  for (const nested of objects(root.nestedClips ?? root.nested)) {
    if (typeof nested.start === "number" && nested.start > duration) diagnostics.push(issue("PXSDKSEM1203", "Nested clip starts after the parent duration", path));
  }
  return diagnostics;
}

function uiSemantics(root: ObjectValue, path?: string): AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  const nodes = objects(root.nodes);
  diagnostics.push(...uniqueIdentities(nodes, "id", "UI node", path));
  const nodeIds = new Set(nodes.map((node) => node.id).filter((id): id is string => typeof id === "string"));
  if (!nodeIds.has(root.rootId as string)) diagnostics.push(issue("PXSDKSEM1301", "UI rootId must identify a node", path));
  const parentById = new Map<string, string | null>();
  const siblingOrders = new Set<string>();
  for (const node of nodes) {
    if (typeof node.id !== "string") continue;
    const parent = typeof node.parentId === "string" ? node.parentId : null;
    parentById.set(node.id, parent);
    if (parent !== null && !nodeIds.has(parent)) diagnostics.push(issue("PXSDKSEM1302", "UI node parentId is unresolved", path, node.id));
    const orderKey = `${parent ?? "<root>"}:${String(node.order)}`;
    if (siblingOrders.has(orderKey)) diagnostics.push(issue("PXSDKSEM1303", "Sibling UI order must be unique", path, orderKey));
    siblingOrders.add(orderKey);
  }
  for (const id of nodeIds) {
    const visited = new Set<string>();
    let cursor: string | null | undefined = id;
    while (cursor !== null && cursor !== undefined) {
      if (visited.has(cursor)) {
        diagnostics.push(issue("PXSDKSEM1304", "UI hierarchy contains a cycle", path, id));
        break;
      }
      visited.add(cursor);
      cursor = parentById.get(cursor);
    }
  }
  const groups = objects(root.visualStateGroups);
  diagnostics.push(...uniqueIdentities(groups, "id", "Visual State Group", path));
  for (const group of groups) {
    const states = objects(group.states);
    diagnostics.push(...uniqueIdentities(states, "id", `Visual State in ${String(group.id)}`, path));
    const stateIds = new Set(states.map((state) => state.id).filter((id): id is string => typeof id === "string"));
    if (!stateIds.has(group.defaultState as string)) diagnostics.push(issue("PXSDKSEM1310", "Visual State Group defaultState is unresolved", path, String(group.id)));
    for (const state of states) {
      for (const override of objects(state.overrides)) {
        if (!nodeIds.has(override.nodeId as string)) diagnostics.push(issue("PXSDKSEM1311", "Visual State override targets an unknown node", path, String(override.nodeId)));
      }
    }
    for (const transition of objects(group.transitions)) {
      if (!stateIds.has(transition.from as string) || !stateIds.has(transition.to as string)) diagnostics.push(issue("PXSDKSEM1312", "Visual State transition references an unknown state", path, String(group.id)));
    }
  }
  return diagnostics;
}

export function validateSemantics(contractId: string, value: unknown, path?: string): readonly AuthoringDiagnostic[] {
  const root = object(value);
  if (root === undefined) return [];
  const diagnostics: AuthoringDiagnostic[] = [];
  switch (contractId) {
    case "runtimeIr": {
      const operations = objects(root.operations);
      diagnostics.push(...uniqueIdentities(operations, "operationId", "Runtime operation", path));
      diagnostics.push(...uniqueIdentities(operations, "sourceId", "Runtime source", path));
      break;
    }
    case "sourceMap": {
      const mappings = objects(root.mappings);
      diagnostics.push(...uniqueIdentities(mappings, "operationId", "Source map operation", path));
      diagnostics.push(...uniqueIdentities(mappings, "sourceId", "Source map source", path));
      break;
    }
    case "project": {
      const locales = Array.isArray(root.supportedLocales) ? root.supportedLocales : [];
      if (typeof root.defaultLocale === "string" && !locales.includes(root.defaultLocale)) diagnostics.push(issue("PXSDKSEM1101", "Project defaultLocale must be listed in supportedLocales", path));
      diagnostics.push(...uniqueIdentities(objects(root.assets), "id", "Project asset", path));
      diagnostics.push(...uniqueIdentities(objects(root.characters), "id", "Project character", path));
      break;
    }
    case "character": {
      const expressions = objects(root.expressions);
      diagnostics.push(...uniqueIdentities(expressions, "id", "Character expression", path));
      const aliases = [...(Array.isArray(root.aliases) ? root.aliases : []), ...expressions.flatMap((expression) => Array.isArray(expression.aliases) ? expression.aliases : [])];
      for (const alias of duplicates(aliases)) diagnostics.push(issue("PXSDKSEM1102", "Character aliases must be unique", path, alias));
      if (typeof root.defaultExpressionId === "string" && !expressions.some((expression) => expression.id === root.defaultExpressionId)) diagnostics.push(issue("PXSDKSEM1103", "Character defaultExpressionId is unresolved", path));
      break;
    }
    case "storyIndex": {
      const scenes = objects(root.scenes);
      diagnostics.push(...uniqueIdentities(scenes, "id", "Story scene", path));
      const sceneIds = new Set(scenes.map((scene) => scene.id));
      if (!sceneIds.has(root.entryScene)) diagnostics.push(issue("PXSDKSEM1104", "Story entryScene is unresolved", path));
      for (const chapter of objects(root.chapters)) for (const id of Array.isArray(chapter.scenes) ? chapter.scenes : []) if (!sceneIds.has(id)) diagnostics.push(issue("PXSDKSEM1105", "Chapter references an unknown scene", path, String(id)));
      break;
    }
    case "game": {
      const variables = objects(root.variables);
      diagnostics.push(...uniqueIdentities(variables, "name", "Game variable", path));
      for (const variable of variables) if (!variableDefaultMatches(variable)) diagnostics.push(issue("PXSDKSEM1106", "Game variable default does not match its declared type", path, String(variable.name)));
      break;
    }
    case "extension": {
      const commands = objects(root.commands);
      const actions = objects(root.actions);
      const inheritedSafety = hasCompleteSafety(root.safety);
      for (const id of duplicates([...commands, ...actions].map((item) => item.id))) diagnostics.push(issue("PXSDKSEM1107", "Extension command and Action ids must be unique", path, id));
      for (const declaration of [...commands, ...actions]) {
        const parameters = objects(declaration.parameters);
        diagnostics.push(...uniqueIdentities(parameters, "name", `Extension parameter in ${String(declaration.id)}`, path));
        if (!inheritedSafety && !hasCompleteSafety(declaration.safety)) {
          diagnostics.push(issue("PXSDKSEM1109", "Extension command and Action require an explicit or inherited safety contract", path, String(declaration.id ?? "")));
        }
        for (const parameter of parameters) {
          const range = object(parameter.range);
          if (range !== undefined && typeof range.minimum === "number" && typeof range.maximum === "number" && range.minimum > range.maximum) diagnostics.push(issue("PXSDKSEM1108", "Extension parameter minimum exceeds maximum", path, `${String(declaration.id)}.${String(parameter.name)}`));
        }
      }
      break;
    }
    case "timeline":
    case "animation": diagnostics.push(...timelineSemantics(root, path)); break;
    case "ui": diagnostics.push(...uiSemantics(root, path)); break;
  }
  return diagnostics;
}

export interface UiPropertyMetadata {
  readonly id: string;
  readonly writable: boolean;
  readonly bindable: boolean;
  readonly animatable: boolean;
}

export interface UiControlMetadata {
  readonly runtimeType: string;
  readonly properties: readonly UiPropertyMetadata[];
}

export interface UiTypeRegistryMetadata {
  readonly controls: readonly UiControlMetadata[];
}

export function validateUiPropertyUsage(value: unknown, registry: UiTypeRegistryMetadata, path?: string): readonly AuthoringDiagnostic[] {
  const root = object(value);
  if (root === undefined) return [];
  const diagnostics: AuthoringDiagnostic[] = [];
  const metadata = new Map(registry.controls.map((control) => [control.runtimeType, new Map(control.properties.map((property) => [property.id, property]))]));
  const nodes = objects(root.nodes);
  const runtimeTypeByNode = new Map(nodes.map((node) => [node.id, node.runtimeType]));
  const check = (nodeId: unknown, propertyId: unknown, capability: keyof Pick<UiPropertyMetadata, "writable" | "bindable" | "animatable">, code: string): void => {
    const runtimeType = runtimeTypeByNode.get(nodeId);
    const property = typeof runtimeType === "string" && typeof propertyId === "string" ? metadata.get(runtimeType)?.get(propertyId) : undefined;
    if (property === undefined || !property[capability]) diagnostics.push(issue(code, `UI property is not ${capability}`, path, `${String(nodeId)}.${String(propertyId)}`));
  };
  for (const node of nodes) {
    for (const property of Object.keys(object(node.runtimeProperties) ?? {})) check(node.id, property, "writable", "PXSDKSEM1320");
    for (const property of Object.keys(object(node.bindings) ?? {})) check(node.id, property, "bindable", "PXSDKSEM1321");
  }
  for (const group of objects(root.visualStateGroups)) for (const state of objects(group.states)) for (const override of objects(state.overrides)) check(override.nodeId, override.property, "writable", "PXSDKSEM1322");
  for (const clip of objects(object(root.animations)?.clips)) for (const track of objects(clip.tracks)) check(track.nodeId, track.property, "animatable", "PXSDKSEM1323");
  return diagnostics;
}
