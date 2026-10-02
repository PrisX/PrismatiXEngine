import type {AuthoringDiagnostic} from "./types.js";
import {playerActionNamespaces, playerActions} from "./player-actions.js";
import {isSafeResourcePath} from "./resources.js";
import {nativeJsonValueValid, nativeUiValueType} from "./ui-value.js";
import {jsonPointerSpans} from "./json-source-spans.js";

type ObjectValue = Record<string, unknown>;

function object(value: unknown): ObjectValue | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value) ? value as ObjectValue : undefined;
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

function manifestActionDefaultMatches(type: unknown, value: unknown): boolean {
  if (value === null) return true; // Native treats a null default as absent.
  const finite = (item: unknown) => typeof item === "number" && Number.isFinite(item);
  const integer = (item: unknown) => typeof item === "number" && Number.isSafeInteger(item);
  const uuid = (item: unknown) => typeof item === "string" &&
    /^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/iu.test(item);
  const shallowScalar = (item: unknown) => typeof item === "string" ||
    typeof item === "boolean" || finite(item);
  switch (type) {
    case "boolean": return typeof value === "boolean";
    case "integer": return integer(value);
    case "number": return finite(value);
    case "string": case "token": return typeof value === "string";
    case "uuid": return uuid(value);
    case "resource": {
      if (typeof value === "string") return true;
      const reference = object(value);
      return typeof reference?.path === "string" &&
        (reference.id === undefined || uuid(reference.id));
    }
    case "vec2": return Array.isArray(value) && value.length === 2 && value.every(finite);
    case "rect": return Array.isArray(value) && value.length === 4 && value.every(finite);
    case "color": return Array.isArray(value) && value.length === 4 &&
      value.every((channel) => integer(channel) && channel >= 0 && channel <= 255);
    case "array": return Array.isArray(value) && value.every(shallowScalar);
    case "object": {
      const record = object(value);
      return record !== undefined && Object.values(record).every(shallowScalar);
    }
    default: return false;
  }
}

function validActionEditorHint(hint: unknown, type: unknown, hasEnum: boolean): boolean {
  if (hint === undefined || hint === "default") return true;
  if (hint === "multiline" || hint === "route") return type === "string";
  if (hint === "enum") return type === "string" && hasEnum;
  if (hint === "color") return type === "color";
  if (hint === "resource") return type === "resource";
  if (hint === "node") return type === "uuid";
  if (hint === "animation") return type === "resource" || type === "string";
  if (hint === "token") return type === "token";
  return false;
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

function motionSemantics(root: ObjectValue, path?: string): AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  const tickRate = typeof root.tickRate === "number" ? root.tickRate : 1;
  const duration = typeof root.duration === "number" ? root.duration : 0;
  if (duration / tickRate < 0.000001 || duration / tickRate > 86400)
    diagnostics.push(issue("PXSDKSEM1416", "Motion duration must be between 0.000001 and 86400 seconds after tick conversion", path));
  const layers = objects(root.layers);
  diagnostics.push(...uniqueIdentities(layers, "id", "Motion layer", path));
  const byId = new Map(layers.map((layer) => [layer.id, layer]));
  for (const layer of layers) {
    if (typeof layer.timeOffset === "number" &&
        Math.abs(layer.timeOffset / tickRate) > 86400)
      diagnostics.push(issue("PXSDKSEM1417", "Motion layer timeOffset exceeds 86400 seconds after tick conversion", path, String(layer.id)));
    if (layer.mask !== undefined) {
      const source = byId.get(layer.mask);
      if (source === undefined)
        diagnostics.push(issue("PXSDKSEM1412", "Motion mask layer does not exist", path, String(layer.id)));
      else if (source.kind === "group")
        diagnostics.push(issue("PXSDKSEM1415", "Motion group cannot supply image alpha for a mask", path, String(layer.id)));
      else if (source === layer || source.mask !== undefined)
        diagnostics.push(issue("PXSDKSEM1413", "Motion masks cannot reference themselves or another masked layer", path, String(layer.id)));
      if (layer.blendMode !== undefined && layer.blendMode !== "normal")
        diagnostics.push(issue("PXSDKSEM1414", "Masked motion layers currently require normal blending", path, String(layer.id)));
      if (source !== undefined && source.blendMode !== undefined && source.blendMode !== "normal")
        diagnostics.push(issue("PXSDKSEM1414", "Motion mask sources require normal blending", path, String(layer.id)));
    }
    if (typeof layer.image === "string" &&
        (layer.image.includes(":") || /[\x00-\x1f]/u.test(layer.image) ||
         /\.(?:pxmotion|pxportrait)$/iu.test(layer.image) ||
         layer.image.split("/").some((part) => part === "" || part === "." || part === "..")))
      diagnostics.push(issue("PXSDKSEM1407", "Motion image must use a safe project-relative texture path", path, String(layer.id)));
    if (Array.isArray(layer.frames))
      for (const [index, frame] of layer.frames.entries()) {
        const rectangle = object(frame);
        if (rectangle !== undefined &&
            (Number(rectangle.x) + Number(rectangle.width) > 16384 ||
             Number(rectangle.y) + Number(rectangle.height) > 16384))
          diagnostics.push(issue("PXSDKSEM1409", "Motion atlas frame exceeds the texture coordinate limit",
            path, `${String(layer.id)}.frames[${index}]`));
      }
    if (layer.parent === undefined) continue;
    if (!byId.has(layer.parent)) {
      diagnostics.push(issue("PXSDKSEM1401", "Motion layer parent does not exist", path, String(layer.id)));
      continue;
    }
    const seen = new Set<unknown>([layer.id]);
    let cursor: ObjectValue | undefined = layer;
    while (cursor?.parent !== undefined) {
      if (seen.has(cursor.parent)) {
        diagnostics.push(issue("PXSDKSEM1402", "Motion layer hierarchy contains a cycle", path, String(layer.id)));
        break;
      }
      seen.add(cursor.parent);
      cursor = byId.get(cursor.parent);
    }
  }
  const tracks = objects(root.tracks);
  const targets = new Set<string>();
  // Keys live on a layer's local clock. A sped-up child may have
  // keys beyond the enclosing clip's duration while still being visible.
  const keyTimeLimit = 86400 * tickRate;
  for (const track of tracks) {
    const target = `${String(track.layer)}.${String(track.property)}`;
    const targetLayer = byId.get(track.layer);
    if (targetLayer === undefined)
      diagnostics.push(issue("PXSDKSEM1403", "Motion track references an unknown layer", path, target));
    if (targets.has(target))
      diagnostics.push(issue("PXSDKSEM1404", "Motion property has multiple tracks", path, target));
    targets.add(target);
    let previous = -1;
    for (const key of objects(track.keys)) {
      const tint = object(key.value);
      const tintValid = tint !== undefined && [tint.r, tint.g, tint.b].every((channel) =>
        typeof channel === "number" && Number.isFinite(channel) && channel >= 0 && channel <= 1);
      if (typeof key.time !== "number" || key.time <= previous || key.time > keyTimeLimit)
        diagnostics.push(issue("PXSDKSEM1405",
          "Motion keys must have increasing times inside the revision's time limit", path, target));
      if (track.property === "visible"
        ? typeof key.value !== "boolean" || key.curve !== "step"
        : track.property === "frame"
          ? typeof key.value !== "number" || !Number.isInteger(key.value) ||
            key.curve !== "step" ||
            !Array.isArray(targetLayer?.frames) ||
            Number(key.value) < 0 || Number(key.value) >= targetLayer.frames.length
          : track.property === "tint"
            ? !tintValid
          : typeof key.value !== "number" || !Number.isFinite(key.value))
        diagnostics.push(issue("PXSDKSEM1406", "Motion key value or curve does not match its property", path, target));
      if (typeof key.value === "number" && track.property !== "frame") {
        const limit = track.property === "opacity" ? 1 :
          track.property === "scaleX" || track.property === "scaleY" ? 1000 : 1000000;
        const minimum = track.property === "opacity" ? 0 : -limit;
        if (key.value < minimum || key.value > limit)
          diagnostics.push(issue("PXSDKSEM1408", "Motion key value exceeds its property range", path, target));
      }
      previous = typeof key.time === "number" ? key.time : previous;
    }
  }
  const markers = objects(root.markers);
  diagnostics.push(...uniqueIdentities(markers, "id", "Motion marker", path));
  let previousMarkerTime = -1;
  for (const marker of markers) {
    if (typeof marker.time !== "number" || marker.time < previousMarkerTime ||
        marker.time > duration)
      diagnostics.push(issue("PXSDKSEM1410",
        "Motion markers must be ordered and inside duration", path, String(marker.id)));
    if (typeof marker.audio === "string" &&
        /\.(?:pxmotion|pxportrait)$/iu.test(marker.audio))
      diagnostics.push(issue("PXSDKSEM1411",
        "Motion marker audio must reference an audio resource", path, String(marker.id)));
    if (typeof marker.time === "number") previousMarkerTime = marker.time;
  }
  return diagnostics;
}

const structuralUiKinds = new Set(["control", "group", "stack", "hbox", "vbox", "grid"]);
const layoutOwningUiKinds = new Set(["stack", "hbox", "vbox", "grid"]);

function validBuiltinUiAction(value: unknown, inItemTemplate = false): boolean {
  const action = object(value);
  if (action === undefined || typeof action.id !== "string") return true;
  const spec = playerActions[action.id];
  // Extensions and project-specific actions are checked by their catalogs.
  if (spec === undefined) return true;
  const args = object(action.arguments) ?? {};
  const keys = Object.keys(args);
  if (spec.argument === undefined) return keys.length === 0;
  if (keys.length !== 1 || keys[0] !== spec.argument.key) return false;
  const argument = args[spec.argument.key];
  switch (spec.argument.type) {
    case "integer":
      return typeof argument === "number" && Number.isSafeInteger(argument) ||
        inItemTemplate && (argument === "$index" || argument === "$key");
    case "boolean": return typeof argument === "boolean";
    case "string": return typeof argument === "string";
  }
}

/** A Player-namespace id that is not a Player action, e.g. "set.bgm.vaule". */
function unknownPlayerAction(value: unknown): string | undefined {
  const id = object(value)?.id;
  if (typeof id !== "string" || id in playerActions) return undefined;
  const namespace = id.slice(0, Math.max(0, id.indexOf(".")));
  return playerActionNamespaces.has(namespace) ? id : undefined;
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
    case "array": return Array.isArray(value) && nativeJsonValueValid(value);
    case "object": return record !== undefined && nativeJsonValueValid(value);
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

  const checkMetadata = (owner: ObjectValue, location: string): void => {
    if (owner.metadata !== undefined &&
        (object(owner.metadata) === undefined || !nativeJsonValueValid(owner.metadata)))
      diagnostics.push(issue("PXSDKUICOMP1108",
        "UI component metadata exceeds native JSON limits", path, location));
  };
  checkMetadata(componentInterface, "componentInterface.metadata");

  const properties = objects(componentInterface.properties);
  diagnostics.push(...uniqueIdentities(properties, "id", "UI component exposed property", path));
  for (const [index, property] of properties.entries()) {
    checkMetadata(property, `componentInterface.properties[${index}].metadata`);
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
  for (const [index, signal] of signals.entries()) {
    checkMetadata(signal, `componentInterface.signals[${index}].metadata`);
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
  for (const [index, slot] of slots.entries()) {
    checkMetadata(slot, `componentInterface.slots[${index}].metadata`);
    const node = typeof slot.nodeId === "string" ? byId.get(slot.nodeId) : undefined;
    if (node === undefined || node.kind === "button" || node.kind === "label" || node.kind === "image") {
      diagnostics.push(issue("PXSDKUICOMP1107", "UI component slot is invalid", path, String(slot.id ?? "")));
    }
  }
  return diagnostics;
}

function validateNativeUiValues(root: ObjectValue, path?: string): AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  const check = (value: unknown, location: string, action = false): void => {
    if (nativeUiValueType(value, action) === "invalid")
      diagnostics.push(issue("PXSDKSEM1325",
        `UI ${action ? "Action argument" : "value"} cannot be parsed by the native loader`,
        path, location));
  };
  const checkMap = (value: unknown, location: string, action = false): void => {
    for (const [key, item] of Object.entries(object(value) ?? {}))
      check(item, `${location}.${key}`, action);
  };

  for (const [index, node] of objects(root.nodes).entries()) {
    checkMap(node.runtimeProperties, `nodes[${index}].runtimeProperties`);
    checkMap(object(node.onClick)?.arguments, `nodes[${index}].onClick.arguments`, true);
    const signals = object(object(node.componentInstance)?.publicSignals) ?? {};
    for (const [name, signal] of Object.entries(signals))
      checkMap(object(signal)?.arguments,
        `nodes[${index}].componentInstance.publicSignals.${name}.arguments`, true);
  }
  for (const [index, node] of objects(object(root.behaviorGraph)?.nodes).entries()) {
    checkMap(node.properties, `behaviorGraph.nodes[${index}].properties`);
    checkMap(node.arguments, `behaviorGraph.nodes[${index}].arguments`);
  }
  for (const [groupIndex, group] of objects(root.visualStateGroups).entries())
    for (const [stateIndex, state] of objects(group.states).entries())
      for (const [overrideIndex, override] of objects(state.overrides).entries())
        check(override.value,
          `visualStateGroups[${groupIndex}].states[${stateIndex}].overrides[${overrideIndex}].value`);
  const animations = object(root.animations);
  for (const [clipIndex, clip] of objects(animations?.clips).entries())
    for (const [trackIndex, track] of objects(clip.tracks).entries())
      for (const [keyIndex, key] of objects(track.keys).entries())
        check(key.value,
          `animations.clips[${clipIndex}].tracks[${trackIndex}].keys[${keyIndex}].value`);
  const machine = object(animations?.stateMachine);
  for (const [index, parameter] of objects(machine?.parameters).entries())
    check(parameter.defaultValue, `animations.stateMachine.parameters[${index}].defaultValue`);
  for (const [transitionIndex, transition] of objects(machine?.transitions).entries())
    for (const [conditionIndex, condition] of objects(transition.conditions).entries())
      check(condition.value,
        `animations.stateMachine.transitions[${transitionIndex}].conditions[${conditionIndex}].value`);
  return diagnostics;
}

function uiSemantics(root: ObjectValue, path?: string): AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  diagnostics.push(...validateNativeUiValues(root, path));
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
      diagnostics.push(issue("PXSDKUI1018", "UI leaf nodes require a runtimeType", path, node.id));
    }
    if (typeof node.runtimeType === "string" && !/^[A-Za-z0-9_.-]*$/u.test(node.runtimeType)) {
      diagnostics.push(issue("PXSDKUI1018", "UI node runtimeType is invalid", path, node.id));
    }
    const orderKey = `${parent ?? "<root>"}:${String(node.order)}`;
    if (siblingOrders.has(orderKey)) diagnostics.push(issue("PXSDKSEM1303", "Sibling UI order must be unique", path, orderKey));
    siblingOrders.add(orderKey);
  }
  const inItemTemplate = (nodeId: string): boolean => {
    const seen = new Set<string>();
    let parent = parentById.get(nodeId);
    while (parent !== null && parent !== undefined && !seen.has(parent)) {
      seen.add(parent);
      if (object(nodes.find((node) => node.id === parent)?.itemSource) !== undefined) return true;
      parent = parentById.get(parent);
    }
    return false;
  };
  for (const node of nodes) {
    if (typeof node.id !== "string") continue;
    if (node.runtimeType === "SpriteText") {
      const properties = object(node.runtimeProperties);
      const glyphs = properties?.glyphs;
      const seenGlyphs = new Set<string>();
      const singleGrapheme = new Intl.Segmenter("und", {granularity: "grapheme"});
      if (!Array.isArray(glyphs) || glyphs.length < 1 || glyphs.length > 128)
        diagnostics.push(issue("PXSDKUISPRITE1001", "SpriteText requires 1 to 128 glyph entries", path, node.id));
      else for (const [index, value] of glyphs.entries()) {
        const glyph = object(value);
        const character = glyph?.character;
        const validCharacter = typeof character === "string" &&
          [...singleGrapheme.segment(character)].length === 1;
        if (glyph === undefined || Object.keys(glyph).length !== 6 ||
            !validCharacter || seenGlyphs.has(character) ||
            typeof glyph.path !== "string" || !isSafeResourcePath(glyph.path) ||
            !["x", "y", "width", "height"].every((field) =>
              typeof glyph[field] === "number" && Number.isFinite(glyph[field]) &&
              Math.abs(glyph[field]) <= 4096) ||
            typeof glyph.width !== "number" || glyph.width <= 0 ||
            typeof glyph.height !== "number" || glyph.height <= 0)
          diagnostics.push(issue("PXSDKUISPRITE1002", "SpriteText glyph entry is invalid or duplicated", path,
            `${node.id}: glyphs[${index}]`));
        if (validCharacter) seenGlyphs.add(character);
      }
      if (properties?.text !== undefined &&
          (typeof properties.text !== "string" ||
           new TextEncoder().encode(properties.text).length > 1024))
        diagnostics.push(issue("PXSDKUISPRITE1003", "SpriteText text must be at most 1024 UTF-8 bytes", path, node.id));
      if (properties?.advance !== undefined &&
          (typeof properties.advance !== "number" || !Number.isFinite(properties.advance) ||
           properties.advance < 0 || properties.advance > 4096))
        diagnostics.push(issue("PXSDKUISPRITE1004", "SpriteText advance must be between 0 and 4096", path, node.id));
    }
    const unknownAction = unknownPlayerAction(node.onClick);
    if (unknownAction !== undefined)
      diagnostics.push(issue("PXSDKUI1026", "UI action is not a Player action", path,
        `${String(node.name ?? node.id)}: ${unknownAction}`));
    else if (!validBuiltinUiAction(node.onClick, inItemTemplate(node.id))) {
      const spec = playerActions[String(object(node.onClick)?.id)];
      const expected = spec?.argument === undefined ? "no arguments"
        : `arguments {${spec.argument.key}: ${spec.argument.type}}`;
      diagnostics.push(issue("PXSDKUI1025", "UI node typed action is invalid", path,
        `${String(node.name ?? node.id)}: ${String(object(node.onClick)?.id)} takes ${expected}`));
    }
    if (object(node.itemSource) !== undefined) {
      if (node.runtimeType !== "GridView" && node.runtimeType !== "ListView" &&
          node.runtimeType !== "PlacedItemsView")
        diagnostics.push(issue("PXSDKUI1190", "UI itemSource requires GridView, ListView, or PlacedItemsView", path, node.id));
      const placement = object(node.itemSource)?.placement;
      if (node.runtimeType === "PlacedItemsView" ? typeof placement !== "string" : placement !== undefined)
        diagnostics.push(issue("PXSDKUI1190", "UI placement is required only for PlacedItemsView", path, node.id));
      const children = nodes.filter((candidate) => candidate.parentId === node.id);
      if (children.length !== 1 || !structuralUiKinds.has(String(children[0]?.kind)))
        diagnostics.push(issue("PXSDKUI1191", "UI itemSource requires one structural child template", path, node.id));
      if (inItemTemplate(node.id))
        diagnostics.push(issue("PXSDKUI1191", "Nested UI itemSource templates are not supported", path, node.id));
    }
    if (inItemTemplate(node.id)) {
      for (const binding of Object.values(object(node.bindings) ?? {})) {
        const bindingPath = object(binding)?.path;
        if (typeof bindingPath !== "string" || !bindingPath.startsWith("item."))
          diagnostics.push(issue("PXSDKUI1193", "UI item template bindings must use item.<field>", path, node.id));
      }
    }
  }
  const templateNodeIds = new Set(nodes.filter((node) =>
    typeof node.id === "string" && inItemTemplate(node.id)).map((node) => node.id));
  const rejectTemplateTarget = (nodeId: unknown, section: string): void => {
    if (templateNodeIds.has(nodeId))
      diagnostics.push(issue("PXSDKUI1194",
        "Behavior, Visual State, and Animation cannot target repeated UI items", path,
        `${section}: ${String(nodeId)}`));
  };
  for (const trigger of objects(root.behaviorTriggers))
    rejectTemplateTarget(trigger.nodeId, "behaviorTriggers");
  for (const behavior of objects(object(root.behaviorGraph)?.nodes))
    for (const value of [...Object.values(object(behavior.properties) ?? {}),
      ...Object.values(object(behavior.arguments) ?? {})]) {
      const reference = object(value);
      if (reference?.type === "nodeReference")
        rejectTemplateTarget(reference.nodeId, "behaviorGraph");
    }
  for (const group of objects(root.visualStateGroups))
    for (const state of objects(group.states))
      for (const override of objects(state.overrides))
        rejectTemplateTarget(override.nodeId, "visualStateGroups");
  for (const clip of objects(object(root.animations)?.clips))
    for (const track of objects(clip.tracks))
      rejectTemplateTarget(track.nodeId, "animations");
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
        if (displayName === undefined || displayName.length === 0 || displayName.length > 256) {
          diagnostics.push(issue("PXSDKSEM1111", "Project character displayName must be present and at most 256 characters", path, id));
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
    case "portrait": {
      for (const layer of objects(root.layers)) {
        if (typeof layer.path === "string" &&
            layer.path.toLowerCase().endsWith(".pxportrait"))
          diagnostics.push(issue("PXSDKSEM1350",
            "A portrait layer must reference a texture, not another .pxportrait descriptor",
            path, layer.path));
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
      const inputActions = objects(root.inputActions);
      diagnostics.push(...uniqueIdentities(inputActions, "id", "Game input action", path));
      const legacyActions = new Set(objects(root.inputBindings)
        .filter((binding) => binding.command === "screen.open" && typeof binding.argument === "string")
        .map((binding) => `screen.open.${String(binding.argument)}`));
      for (const action of inputActions) if (legacyActions.has(action.id as string))
        diagnostics.push(issue("PXSDKSEM1121", "Game input action duplicates a legacy screen.open binding", path, String(action.id)));
      for (const action of inputActions) if (action.route !== undefined && action.context !== "game")
        diagnostics.push(issue("PXSDKSEM1122", "Player route input actions require the game context", path, String(action.id)));
      for (const action of inputActions) if (action.route !== undefined && action.uiAction !== undefined)
        diagnostics.push(issue("PXSDKSEM1123", "Input action cannot open a route and dispatch a UI command together", path, String(action.id)));
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
      const capabilities = new Set(Array.isArray(root.capabilities) ? root.capabilities : []);
      for (const action of actions) {
        for (const capability of Array.isArray(action.capabilities) ? action.capabilities : []) {
          if (!capabilities.has(capability))
            diagnostics.push(issue("PXSDKSEM1114", "Extension Action capability is not declared by its manifest",
              path, `${String(action.id)}.${String(capability)}`));
        }
        for (const parameter of objects(action.parameters)) {
          const location = `${String(action.id)}.${String(parameter.name)}`;
          if (parameter.type === "null")
            diagnostics.push(issue("PXSDKSEM1115", "Extension Action parameters cannot use the null type",
              path, location));
          if (parameter.default !== undefined &&
              !manifestActionDefaultMatches(parameter.type, parameter.default))
            diagnostics.push(issue("PXSDKSEM1116", "Extension Action parameter default has the wrong Runtime type",
              path, location));
          const choices = Array.isArray(parameter.enum) ? parameter.enum : [];
          if (choices.length > 0 && (parameter.type !== "string" ||
              choices.some((choice) => typeof choice !== "string")))
            diagnostics.push(issue("PXSDKSEM1117", "Extension Action enum requires string parameters and string choices",
              path, location));
          if (!validActionEditorHint(parameter.editorHint, parameter.type, choices.length > 0) ||
              (parameter.resourceFilter !== undefined && parameter.type !== "resource"))
            diagnostics.push(issue("PXSDKSEM1118", "Extension Action editor hint or resource filter does not match its type",
              path, location));
        }
      }
      break;
    }
    case "timeline":
    case "animation": diagnostics.push(...timelineSemantics(root, path)); break;
    case "motion": diagnostics.push(...motionSemantics(root, path)); break;
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
  /** Native TypeRegistry valueType, when available in the exported manifest. */
  readonly valueType?: string;
  readonly tokenBindable?: boolean;
  /** Native TypeRegistry's accepted choices for a string enum property. */
  readonly enumChoices?: readonly string[];
}

export interface UiControlMetadata {
  readonly runtimeType: string;
  readonly properties: readonly UiPropertyMetadata[];
}

export interface UiTypeRegistryMetadata {
  readonly controls: readonly UiControlMetadata[];
}

function uiValueMatches(value: unknown, property: UiPropertyMetadata): boolean {
  if (property.valueType === undefined) return true;
  const actual = nativeUiValueType(value);
  if (actual === "token" && property.valueType !== "token") return property.tokenBindable === true;
  if (property.valueType === "number" && actual === "integer") return true;
  return actual === property.valueType;
}

export function validateUiPropertyUsage(value: unknown, registry: UiTypeRegistryMetadata,
  path?: string, source?: string): readonly AuthoringDiagnostic[] {
  const root = object(value);
  if (root === undefined) return [];
  const diagnostics: AuthoringDiagnostic[] = [];
  let sourceSpans: ReturnType<typeof jsonPointerSpans> | undefined;
  const report = (code: string, message: string, details: string,
    pointer: string): void => {
    if (source !== undefined && path !== undefined)
      sourceSpans ??= jsonPointerSpans(source, value, path);
    const span = sourceSpans?.get(pointer);
    diagnostics.push({...issue(code, message, path, details),
      ...(span === undefined ? {} : {span})});
  };
  const escapePointer = (part: string) => part.replaceAll("~", "~0").replaceAll("/", "~1");
  const metadata = new Map(registry.controls.map((control) => [control.runtimeType, new Map(control.properties.map((property) => [property.id, property]))]));
  const nodes = objects(root.nodes);
  const runtimeTypeByNode = new Map(nodes.map((node) => [node.id, node.runtimeType]));
  const check = (nodeId: unknown, propertyId: unknown, capability: keyof Pick<UiPropertyMetadata, "writable" | "bindable" | "animatable">,
    code: string, pointer: string, authoredValue?: unknown,
    valuePointer = pointer): void => {
    const runtimeType = runtimeTypeByNode.get(nodeId);
    const property = typeof runtimeType === "string" && typeof propertyId === "string" ? metadata.get(runtimeType)?.get(propertyId) : undefined;
    const location = `${String(nodeId)}.${String(propertyId)}`;
    if (property === undefined || !property[capability]) report(code,
      `UI property is not ${capability}`, location, pointer);
    else if (capability === "writable") {
      if (!uiValueMatches(authoredValue, property))
        report("PXSDKSEM1324", `UI property expects ${property.valueType} but received ${nativeUiValueType(authoredValue)}`, location, valuePointer);
      else if (typeof authoredValue === "string" && property.enumChoices?.length &&
          !property.enumChoices.includes(authoredValue))
        report("PXSDKSEM1326", `UI property expects one of ${property.enumChoices.join(", ")}`, location, valuePointer);
    }
  };
  for (const [index, raw] of (Array.isArray(root.nodes) ? root.nodes : []).entries()) {
    const node = object(raw);
    if (node === undefined) continue;
    for (const [property, authoredValue] of Object.entries(object(node.runtimeProperties) ?? {}))
      check(node.id, property, "writable", "PXSDKSEM1320",
        `/nodes/${index}/runtimeProperties/${escapePointer(property)}`, authoredValue);
    for (const property of Object.keys(object(node.bindings) ?? {}))
      check(node.id, property, "bindable", "PXSDKSEM1321",
        `/nodes/${index}/bindings/${escapePointer(property)}`);
  }
  for (const [groupIndex, group] of indexedObjects(root.visualStateGroups))
    for (const [stateIndex, state] of indexedObjects(group.states))
      for (const [overrideIndex, override] of indexedObjects(state.overrides)) {
        const pointer = `/visualStateGroups/${groupIndex}/states/${stateIndex}/overrides/${overrideIndex}`;
        check(override.nodeId, override.property, "writable", "PXSDKSEM1322",
          `${pointer}/property`, override.value, `${pointer}/value`);
      }
  for (const [clipIndex, clip] of indexedObjects(object(root.animations)?.clips))
    for (const [trackIndex, track] of indexedObjects(clip.tracks))
      check(track.nodeId, track.property, "animatable", "PXSDKSEM1323",
        `/animations/clips/${clipIndex}/tracks/${trackIndex}/property`);
  return diagnostics;
}
