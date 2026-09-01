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
    if (found.has(value)) duplicate.add(value);
    else found.add(value);
  }
  return [...duplicate].sort();
}

function caseInsensitiveDuplicates(values: readonly unknown[]): string[] {
  const found = new Map<string, string>();
  const duplicate = new Set<string>();
  for (const value of values) {
    if (typeof value !== "string") continue;
    const folded = value.toLocaleLowerCase("en-US");
    const previous = found.get(folded);
    if (previous !== undefined) duplicate.add(previous);
    else found.set(folded, value);
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

const structuralUiKinds = new Set(["control", "group", "stack", "hbox", "vbox", "grid"]);
const layoutOwningUiKinds = new Set(["stack", "hbox", "vbox", "grid"]);

function boundedJsonValue(value: unknown, depth = 0, budget = {nodes: 0}): boolean {
  if (depth > 32 || ++budget.nodes > 8192) return false;
  if (value === null || typeof value === "boolean" || typeof value === "string") return true;
  if (typeof value === "number") return Number.isFinite(value);
  if (Array.isArray(value)) return value.length <= 1024 && value.every((item) => boundedJsonValue(item, depth + 1, budget));
  const record = object(value);
  if (record === undefined || Object.keys(record).length > 256) return false;
  return Object.values(record).every((item) => boundedJsonValue(item, depth + 1, budget));
}

function componentValueMatches(valueType: unknown, value: unknown): boolean {
  const record = object(value);
  const exactKeys = (expected: readonly string[]) => record !== undefined &&
    Object.keys(record).length === expected.length && expected.every((key) => key in record);
  const finite = (candidate: unknown) => typeof candidate === "number" && Number.isFinite(candidate);
  const uuid = (candidate: unknown) => typeof candidate === "string" &&
    /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/iu.test(candidate);
  switch (valueType) {
    case "null": return value === null;
    case "boolean": return typeof value === "boolean";
    case "integer": return typeof value === "number" && Number.isInteger(value);
    case "number": return finite(value);
    case "string": return typeof value === "string";
    case "uuid": return uuid(value);
    case "resource": return (typeof value === "string" && (value.length === 0 || uuid(value))) ||
      (exactKeys(["type", "value"]) && record?.type === "resource" &&
        typeof record.value === "string" && (record.value.length === 0 || uuid(record.value)));
    case "token": return (typeof value === "string" && value.length > 0) ||
      (exactKeys(["type", "value"]) && record?.type === "token" &&
        typeof record.value === "string" && record.value.length > 0);
    case "array": return Array.isArray(value) && boundedJsonValue(value);
    case "object": return record !== undefined && boundedJsonValue(value);
    case "vec2": return exactKeys(["type", "x", "y"]) && record?.type === "vec2" &&
      finite(record.x) && finite(record.y);
    case "rect": return exactKeys(["type", "x", "y", "width", "height"]) &&
      record?.type === "rect" && finite(record.x) && finite(record.y) &&
      finite(record.width) && finite(record.height);
    case "color": return exactKeys(["type", "value"]) && record?.type === "color" &&
      typeof record.value === "string" && /^#[0-9a-f]{6}(?:[0-9a-f]{2})?$/iu.test(record.value);
    default: return false;
  }
}

function propertyAtPath(root: ObjectValue, propertyPath: unknown): unknown {
  if (typeof propertyPath !== "string" ||
      !/^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/u.test(propertyPath)) return undefined;
  let current: unknown = root;
  for (const segment of propertyPath.split(".")) {
    const record = object(current);
    if (record === undefined || !(segment in record)) return undefined;
    current = record[segment];
  }
  return current;
}

function uiComponentSemantics(root: ObjectValue, path?: string): AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  const nodes = objects(root.nodes);
  const byId = new Map(nodes.flatMap((node) => typeof node.id === "string" ? [[node.id, node] as const] : []));
  const componentInterface = object(root.componentInterface);
  if (componentInterface === undefined) return diagnostics;

  const properties = objects(componentInterface.properties);
  diagnostics.push(...uniqueIdentities(properties, "id", "UI component exposed property", path));
  for (const property of properties) {
    const node = typeof property.nodeId === "string" ? byId.get(property.nodeId) : undefined;
    const authored = node === undefined ? undefined : propertyAtPath(node, property.property);
    if (authored === undefined ||
        !componentValueMatches(property.valueType, authored) ||
        !componentValueMatches(property.valueType, property.defaultValue)) {
      diagnostics.push(issue("PXSDKUICOMP1105", "UI component exposed property is invalid", path, String(property.id ?? "")));
    }
  }

  const controlSignals = new Set(["pointerEntered", "pointerExited", "pointerDown", "pointerUp", "clicked", "scrolled", "focusEntered", "focusExited"]);
  const signals = objects(componentInterface.signals);
  diagnostics.push(...uniqueIdentities(signals, "id", "UI component exposed signal", path));
  for (const signal of signals) {
    const node = typeof signal.nodeId === "string" ? byId.get(signal.nodeId) : undefined;
    const signalName = typeof signal.signal === "string" ? signal.signal : "";
    const dynamic = node !== undefined && typeof node.runtimeType === "string";
    const validBuiltIn = node !== undefined &&
      (controlSignals.has(signalName) || (node.kind === "button" && signalName === "activated"));
    const arguments_ = objects(signal.arguments);
    if (node === undefined || (!dynamic && !validBuiltIn) ||
        duplicates(arguments_.map((argument) => argument.id)).length > 0) {
      diagnostics.push(issue("PXSDKUICOMP1106", "UI component exposed signal is invalid", path, String(signal.id ?? "")));
    }
  }

  const slots = objects(componentInterface.slots);
  diagnostics.push(...uniqueIdentities(slots, "id", "UI component slot", path));
  for (const slot of slots) {
    const node = typeof slot.nodeId === "string" ? byId.get(slot.nodeId) : undefined;
    if (node === undefined || node.kind === "button" || node.kind === "label" || node.kind === "image") {
      diagnostics.push(issue("PXSDKUICOMP1107", "UI component slot is invalid", path, String(slot.id ?? "")));
    }
  }
  return diagnostics;
}

function uiSemantics(root: ObjectValue, path?: string): AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  const nodes = objects(root.nodes);
  diagnostics.push(...uniqueIdentities(nodes, "id", "UI node", path));
  const nodeIds = new Set(nodes.map((node) => node.id).filter((id): id is string => typeof id === "string"));
  if (!nodeIds.has(root.rootId as string)) diagnostics.push(issue("PXSDKSEM1301", "UI rootId must identify a node", path));
  const rootNode = nodes.find((node) => node.id === root.rootId);
  if (rootNode !== undefined && (rootNode.parentId !== null || !structuralUiKinds.has(String(rootNode.kind)))) {
    diagnostics.push(issue("PXSDKUI1040", "UI document root must be a parentless structural control", path));
  }
  if (nodes.filter((node) => node.parentId === null).length !== 1) {
    diagnostics.push(issue("PXSDKUI1041", "UI document requires exactly one root", path));
  }
  const parentById = new Map<string, string | null>();
  const siblingOrders = new Set<string>();
  for (const node of nodes) {
    if (typeof node.id !== "string") continue;
    const parent = typeof node.parentId === "string" ? node.parentId : null;
    parentById.set(node.id, parent);
    if (parent !== null && !nodeIds.has(parent)) diagnostics.push(issue("PXSDKSEM1302", "UI node parentId is unresolved", path, node.id));
    const parentNode = parent === null ? undefined : nodes.find((candidate) => candidate.id === parent);
    if (parentNode !== undefined && layoutOwningUiKinds.has(String(parentNode.kind)) && object(node.layout)?.mode !== "container") {
      diagnostics.push(issue("PXSDKUI1043", "UI node layout must be container-owned", path, node.id));
    }
    if (node.kind === "leaf" && typeof node.runtimeType !== "string") {
      diagnostics.push(issue("PXSDKUI1018", "UI leaf nodes require a revision-2 runtimeType", path, node.id));
    }
    if (typeof node.runtimeType === "string" && !/^[A-Za-z0-9_.-]*$/u.test(node.runtimeType)) {
      diagnostics.push(issue("PXSDKUI1018", "UI node runtimeType is invalid", path, node.id));
    }
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
      const entry = root.entry !== null && typeof root.entry === "object" && !Array.isArray(root.entry) ? root.entry as ObjectValue : {};
      const uiEntryPoints = root.uiEntryPoints !== null && typeof root.uiEntryPoints === "object" && !Array.isArray(root.uiEntryPoints) ? root.uiEntryPoints as ObjectValue : {};
      if (typeof entry.ui === "string" && typeof uiEntryPoints[entry.ui] !== "string") diagnostics.push(issue("PXSDKSEM1120", "Project entry.ui must identify a declared UI entry point", path, entry.ui));
      const assets = objects(root.assets);
      const characters = objects(root.characters);
      diagnostics.push(...uniqueIdentities(assets, "id", "Project asset", path));
      diagnostics.push(...uniqueIdentities(characters, "id", "Project character", path));
      for (const source of caseInsensitiveDuplicates(characters.map((character) => character.source))) {
        diagnostics.push(issue("PXSDKSEM1110", "Project character source must be unique ignoring case", path, source));
      }
      for (const character of characters) {
        const id = typeof character.id === "string" ? character.id : undefined;
        const displayName = typeof character.displayName === "string" ? character.displayName : undefined;
        const source = typeof character.source === "string" ? character.source : undefined;
        if (displayName === undefined || displayName.length === 0 || displayName.length > 256) {
          diagnostics.push(issue("PXSDKSEM1111", "Project character displayName must be present and at most 256 characters", path, id));
        }
        if (id !== undefined && source !== undefined) {
          const expected = `Characters/${id}.pxcharacter`;
          if (source.toLocaleLowerCase("en-US") !== expected.toLocaleLowerCase("en-US")) {
            diagnostics.push(issue("PXSDKSEM1112", "Project character source must match its stable UUID", path, expected));
          }
        }
      }
      break;
    }
    case "character": {
      const expressions = objects(root.expressions);
      diagnostics.push(...uniqueIdentities(expressions, "id", "Character expression", path));
      if (typeof root.displayName === "string" && root.displayName.length > 256) {
        diagnostics.push(issue("PXSDKSEM1113", "Character displayName exceeds the Runtime 256-character limit", path));
      }
      for (const name of caseInsensitiveDuplicates(expressions.map((expression) => expression.name))) {
        diagnostics.push(issue("PXSDKSEM1114", "Character expression names must be unique ignoring case", path, name));
      }
      for (const expression of expressions) {
        if (typeof expression.name === "string" && expression.name.length > 256) {
          diagnostics.push(issue("PXSDKSEM1115", "Character expression name exceeds the Runtime 256-character limit", path, String(expression.id ?? "")));
        }
      }
      const characterLookup = [root.id, root.displayName, ...(Array.isArray(root.aliases) ? root.aliases : [])];
      for (const key of duplicates(characterLookup)) diagnostics.push(issue("PXSDKSEM1116", "Character runtime lookup keys must be unambiguous", path, key));
      const expressionLookup = expressions.flatMap((expression) => [expression.id, expression.name, ...(Array.isArray(expression.aliases) ? expression.aliases : [])]);
      for (const key of duplicates(expressionLookup)) diagnostics.push(issue("PXSDKSEM1117", "Character expression runtime lookup keys must be unambiguous", path, key));
      const aliases = [...(Array.isArray(root.aliases) ? root.aliases : []), ...expressions.flatMap((expression) => Array.isArray(expression.aliases) ? expression.aliases : [])];
      for (const alias of duplicates(aliases)) diagnostics.push(issue("PXSDKSEM1102", "Character aliases must be unique", path, alias));
      if (expressions.length > 0 && typeof root.defaultExpressionId !== "string") {
        diagnostics.push(issue("PXSDKSEM1118", "A character with expressions requires defaultExpressionId", path));
      } else if (typeof root.defaultExpressionId === "string" && !expressions.some((expression) => expression.id === root.defaultExpressionId)) {
        diagnostics.push(issue("PXSDKSEM1103", "Character defaultExpressionId is unresolved", path));
      }
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
    case "uiComponent":
      diagnostics.push(...uiSemantics(root, path));
      diagnostics.push(...uiComponentSemantics(root, path));
      break;
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
