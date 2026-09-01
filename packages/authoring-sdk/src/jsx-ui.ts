import {createHash} from "node:crypto";

import {validateDocument} from "./contracts.js";
import {
  Fragment,
  isUiElement,
  uiElementMarker,
  type UiChild,
  type UiElement,
} from "./jsx-runtime.js";
import type {AuthoringDiagnostic, JsonValue, Result} from "./types.js";

export type UiNodeKind = "control" | "label" | "button" | "image" | "stack" |
  "hbox" | "vbox" | "grid" | "group" | "leaf";
export type UiAlignment = "start" | "center" | "end" | "fill";
export type UiSizeRule = "fixed" | "fill" | "content";

export interface UiLayout {
  readonly mode: "free" | "container";
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
  readonly anchorX: number;
  readonly anchorY: number;
  readonly anchorRight: number;
  readonly anchorBottom: number;
  readonly pivotX: number;
  readonly pivotY: number;
  readonly margin: number;
  readonly alignment: UiAlignment;
  readonly sizeRule: UiSizeRule;
}

export type UiLayoutProps = Partial<UiLayout>;

export interface UiPoint {
  readonly x: number;
  readonly y: number;
}

export interface UiSize {
  readonly width: number;
  readonly height: number;
}

export type UiAnchorPreset = "top-left" | "top" | "top-right" | "left" |
  "center" | "right" | "bottom-left" | "bottom" | "bottom-right" | "fill";

export interface UiAppearance {
  readonly backgroundColor?: string;
  readonly textColor?: string;
  readonly opacity?: number;
  readonly styleToken?: string | null;
  readonly hoverBackgroundColor?: string | null;
  readonly focusColor?: string | null;
  readonly disabledOpacity?: number;
}

/** Friendly authoring aliases; these are lowered to canonical appearance fields. */
export interface UiStyle extends UiAppearance {
  readonly background?: string;
  readonly color?: string;
  readonly token?: string | null;
  readonly hoverBackground?: string | null;
  readonly focus?: string | null;
}

export interface UiBinding {
  readonly path: string;
  readonly formatter?: string;
}

export type UiBindings = Readonly<Record<string, UiBinding>>;

export interface UiAction {
  readonly id: string;
  readonly arguments?: Readonly<Record<string, JsonValue>>;
}

export interface UiComponentSignalBinding {
  readonly id: string;
  readonly arguments?: Readonly<Record<string, JsonValue>>;
  readonly argumentBindings?: Readonly<Record<string, string>>;
}

export interface UiComponentInstance {
  readonly componentId: string;
  readonly instanceRootId: string;
  readonly sourceNodeId: string;
  readonly sourcePath: readonly string[];
  readonly overrides?: readonly string[];
  readonly publicProperties?: Readonly<Record<string, JsonValue>>;
  readonly publicSignals?: Readonly<Record<string, UiComponentSignalBinding | null>>;
}

export interface UiComponentSlot {
  readonly instanceRootId: string;
  readonly slotId: string;
}

export interface UiNodeProps {
  /** A canonical UUID is preserved. Any other value is used as a stable ID seed. */
  readonly id?: string;
  /** Stable semantic seed for generated UUIDs. JSX key is also supported. */
  readonly stableId?: string;
  readonly key?: string | number;
  readonly name?: string;
  readonly visible?: boolean;
  readonly locked?: boolean;
  /** Canonical layout escape hatch. Prefer the direct shorthand props below. */
  readonly layout?: UiLayoutProps;
  readonly position?: UiPoint | readonly [x: number, y: number];
  readonly size?: UiSize | readonly [width: number, height: number];
  readonly x?: number;
  readonly y?: number;
  readonly width?: number;
  readonly height?: number;
  readonly margin?: number;
  readonly align?: UiAlignment;
  readonly fill?: boolean;
  readonly anchor?: UiAnchorPreset;
  readonly pivot?: UiPoint | readonly [x: number, y: number];
  /** Canonical appearance escape hatch. `style` offers concise CSS-like aliases. */
  readonly appearance?: UiAppearance;
  readonly style?: UiStyle;
  readonly runtimeProperties?: Readonly<Record<string, JsonValue>>;
  readonly bindings?: UiBindings;
  readonly bind?: Readonly<Record<string, string | UiBinding>>;
  readonly onClick?: UiAction;
  readonly action?: string | UiAction;
  readonly accessibilityLabel?: string;
  readonly accessibilityRole?: string;
  readonly accessibilityDescription?: string;
  readonly accessibilityFocusOrder?: number;
  readonly componentInstance?: UiComponentInstance;
  readonly componentSlot?: UiComponentSlot;
  readonly children?: UiChild;
}

export interface ControlProps extends UiNodeProps {
  readonly kind?: UiNodeKind;
  readonly runtimeType?: string;
}

export interface TextProps extends Omit<UiNodeProps, "children"> {
  readonly text?: string;
  readonly children?: string | number | readonly (string | number)[];
}

export interface ImageProps extends UiNodeProps {
  readonly assetId?: string | null;
}

export interface ButtonProps extends Omit<UiNodeProps, "children"> {
  readonly text?: string;
  readonly children?: string | number | readonly (string | number)[];
}

export interface ThemeToken {
  readonly id: string;
  readonly name: string;
  readonly value: string;
}

export interface UiBehaviorGraph {
  readonly nodes: readonly JsonValue[];
  readonly links: readonly JsonValue[];
  readonly groups: readonly JsonValue[];
}

export interface UiComponentInterface {
  readonly metadata?: Readonly<Record<string, JsonValue>>;
  readonly properties: readonly JsonValue[];
  readonly signals: readonly JsonValue[];
  readonly slots: readonly JsonValue[];
}

interface DocumentProps {
  /** Advanced escape hatch for preserving an existing canonical document UUID. */
  readonly id?: string;
  /** Rename-safe semantic identity for an automatically generated UUID. */
  readonly stableId?: string;
  readonly name: string;
  readonly revision?: number;
  readonly width: number;
  readonly height: number;
  readonly theme?: readonly ThemeToken[];
  readonly children?: UiChild;
}

export interface SceneProps extends DocumentProps {
  readonly behaviorGraph?: UiBehaviorGraph;
  readonly behaviorTriggers?: readonly JsonValue[];
  readonly visualStateGroups?: readonly JsonValue[];
  readonly animations?: JsonValue;
}

export interface ComponentProps extends DocumentProps {
  readonly componentInterface?: UiComponentInterface;
}

export interface PrismatiXUiNode {
  readonly id: string;
  readonly parentId: string | null;
  readonly order: number;
  readonly kind: UiNodeKind;
  readonly runtimeType?: string;
  readonly name: string;
  readonly visible: boolean;
  readonly locked: boolean;
  readonly layout: UiLayout;
  readonly text?: string;
  readonly assetId?: string | null;
  readonly appearance?: UiAppearance;
  readonly onClick?: {readonly id: string; readonly arguments: Readonly<Record<string, JsonValue>>};
  readonly accessibilityLabel?: string;
  readonly accessibilityRole?: string;
  readonly accessibilityDescription?: string;
  readonly accessibilityFocusOrder?: number;
  readonly runtimeProperties: Readonly<Record<string, JsonValue>>;
  readonly bindings: UiBindings;
  readonly componentInstance?: UiComponentInstance;
  readonly componentSlot?: UiComponentSlot;
}

interface PrismatiXUiDocumentBase {
  readonly schemaRevision: 2;
  readonly id: string;
  readonly revision: number;
  readonly name: string;
  readonly width: number;
  readonly height: number;
  readonly rootId: string;
  readonly nodes: readonly PrismatiXUiNode[];
  readonly theme: readonly ThemeToken[];
}

export interface PrismatiXUiScene extends PrismatiXUiDocumentBase {
  readonly format: "PrismatiXUIScene";
  readonly behaviorGraph: UiBehaviorGraph;
  readonly behaviorTriggers: readonly JsonValue[];
  readonly visualStateGroups: readonly JsonValue[];
  readonly animations?: JsonValue;
}

export interface PrismatiXUiComponent extends PrismatiXUiDocumentBase {
  readonly format: "PrismatiXUIComponent";
  readonly componentInterface: UiComponentInterface;
}

export type PrismatiXUiDocument = PrismatiXUiScene | PrismatiXUiComponent;

interface NodeDefinition {
  readonly category: "node";
  readonly kind: UiNodeKind;
  readonly runtimeType?: string;
  readonly props: UiNodeProps & {readonly text?: string; readonly assetId?: string | null};
}

interface DocumentDefinition {
  readonly category: "document";
  readonly format: PrismatiXUiDocument["format"];
  readonly props: SceneProps | ComponentProps;
}

const nodeType = Symbol.for("prismatix.ui.authoring.node");
const documentType = Symbol.for("prismatix.ui.authoring.document");
const uuidPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/iu;

function definitionElement(type: symbol, definition: NodeDefinition | DocumentDefinition): UiElement {
  return {[uiElementMarker]: true, type, props: {definition}};
}

function textualChildren(value: TextProps["children"] | ButtonProps["children"]): string | undefined {
  if (value === undefined) return undefined;
  const values = Array.isArray(value) ? value : [value];
  if (!values.every((item) => typeof item === "string" || typeof item === "number")) {
    throw new TypeError("Text and Button children must be text or numbers");
  }
  return values.join("");
}

function node(
  kind: UiNodeKind,
  runtimeType: string | undefined,
  props: UiNodeProps & {readonly text?: string; readonly assetId?: string | null},
): UiElement {
  return definitionElement(nodeType, {
    category: "node", kind,
    ...(runtimeType === undefined ? {} : {runtimeType}),
    props,
  });
}

export function Scene(props: SceneProps): UiElement {
  return definitionElement(documentType, {category: "document", format: "PrismatiXUIScene", props});
}

/** Root authoring element for a reusable canonical PrismatiXUIComponent. */
export function Component(props: ComponentProps): UiElement {
  return definitionElement(documentType, {category: "document", format: "PrismatiXUIComponent", props});
}

export function Control(props: ControlProps): UiElement {
  return node(props.kind ?? "control", props.runtimeType ?? "Control", props);
}

export function Group(props: UiNodeProps): UiElement {
  return node("group", "Control", props);
}

export function VBox(props: UiNodeProps): UiElement {
  return node("vbox", "VBoxContainer", props);
}

export function HBox(props: UiNodeProps): UiElement {
  return node("hbox", "HBoxContainer", props);
}

export function Grid(props: UiNodeProps): UiElement {
  return node("grid", "GridView", props);
}

export function Text(props: TextProps): UiElement {
  const text = props.text ?? textualChildren(props.children);
  const {children: _children, ...rest} = props;
  return node("label", "Label", {...rest, ...(text === undefined ? {} : {text})});
}

export function Image(props: ImageProps): UiElement {
  return node("image", "TextureRect", props);
}

export function Button(props: ButtonProps): UiElement {
  const text = props.text ?? textualChildren(props.children);
  const {children: _children, ...rest} = props;
  return node("button", "Button", {...rest, ...(text === undefined ? {} : {text})});
}

/** A typed convenience node for already-authored canonical component metadata. */
export function ComponentInstance(props: UiNodeProps & {readonly componentInstance: UiComponentInstance}): UiElement {
  return node("group", "Control", props);
}

/** Marks a node as content supplied to a canonical component slot. */
export function Slot(props: UiNodeProps & {readonly componentSlot: UiComponentSlot}): UiElement {
  return node("group", "Control", props);
}

function diagnostic(code: string, message: string, path?: string, details?: string): AuthoringDiagnostic {
  return {severity: "error", code, message,
    ...(path === undefined ? {} : {path}),
    ...(details === undefined ? {} : {details})};
}

function stableUuid(namespace: string, value: string): string {
  const bytes = createHash("sha256").update(namespace).update("\0").update(value).digest().subarray(0, 16);
  bytes[6] = (bytes[6]! & 0x0f) | 0x50;
  bytes[8] = (bytes[8]! & 0x3f) | 0x80;
  const hex = bytes.toString("hex");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function flattenChildren(value: UiChild, output: UiElement[] = []): UiElement[] {
  if (value === null || value === undefined || typeof value === "boolean") return output;
  if (Array.isArray(value)) {
    for (const child of value) flattenChildren(child, output);
    return output;
  }
  if (typeof value === "string" || typeof value === "number") {
    if (String(value).trim().length === 0) return output;
    throw new TypeError("Text children are only valid inside Text or Button");
  }
  if (!isUiElement(value)) throw new TypeError("JSX children must be PrismatiX UI elements");
  if (value.type === Fragment) return flattenChildren(value.props.children as UiChild, output);
  output.push(value);
  return output;
}

function definition(value: UiElement): NodeDefinition | DocumentDefinition {
  const candidate = value.props.definition;
  if ((value.type !== nodeType && value.type !== documentType) ||
      candidate === null || typeof candidate !== "object") {
    throw new TypeError("Unexpected element in PrismatiX UI authoring tree");
  }
  return candidate as NodeDefinition | DocumentDefinition;
}

function nodeLayout(
  props: UiNodeProps,
  containerOwned: boolean,
  root: boolean,
  width: number,
  height: number,
): UiLayout {
  const point = (value: UiPoint | readonly [number, number] | undefined): UiPoint | undefined =>
    value === undefined ? undefined : Array.isArray(value)
      ? {x: value[0]!, y: value[1]!} : value as UiPoint;
  const size = (value: UiSize | readonly [number, number] | undefined): UiSize | undefined =>
    value === undefined ? undefined : Array.isArray(value)
      ? {width: value[0]!, height: value[1]!} : value as UiSize;
  const anchorPresets: Readonly<Record<UiAnchorPreset, readonly [number, number, number, number]>> = {
    "top-left": [0, 0, 0, 0], top: [0.5, 0, 0.5, 0], "top-right": [1, 0, 1, 0],
    left: [0, 0.5, 0, 0.5], center: [0.5, 0.5, 0.5, 0.5], right: [1, 0.5, 1, 0.5],
    "bottom-left": [0, 1, 0, 1], bottom: [0.5, 1, 0.5, 1],
    "bottom-right": [1, 1, 1, 1], fill: [0, 0, 1, 1],
  };
  const position = point(props.position);
  const authoredSize = size(props.size);
  const pivot = point(props.pivot);
  const anchors = props.anchor === undefined ? undefined : anchorPresets[props.anchor];
  const input: UiLayoutProps = {
    ...(props.fill === true ? {
      anchorX: 0, anchorY: 0, anchorRight: 1, anchorBottom: 1,
      alignment: "fill" as const, sizeRule: "fill" as const,
    } : {}),
    ...(anchors === undefined ? {} : {
      anchorX: anchors[0], anchorY: anchors[1],
      anchorRight: anchors[2], anchorBottom: anchors[3],
    }),
    ...props.layout,
    ...(position === undefined ? {} : {x: position.x, y: position.y}),
    ...(authoredSize === undefined ? {} : {width: authoredSize.width, height: authoredSize.height}),
    ...(pivot === undefined ? {} : {pivotX: pivot.x, pivotY: pivot.y}),
    ...(props.x === undefined ? {} : {x: props.x}),
    ...(props.y === undefined ? {} : {y: props.y}),
    ...(props.width === undefined ? {} : {width: props.width}),
    ...(props.height === undefined ? {} : {height: props.height}),
    ...(props.margin === undefined ? {} : {margin: props.margin}),
    ...(props.align === undefined ? {} : {alignment: props.align}),
  };
  return {
    mode: containerOwned ? "container" : input?.mode ?? "free",
    x: input?.x ?? 0,
    y: input?.y ?? 0,
    width: input?.width ?? (root ? width : 0),
    height: input?.height ?? (root ? height : 0),
    anchorX: input?.anchorX ?? 0,
    anchorY: input?.anchorY ?? 0,
    anchorRight: input?.anchorRight ?? (root ? 1 : 0),
    anchorBottom: input?.anchorBottom ?? (root ? 1 : 0),
    pivotX: input?.pivotX ?? 0,
    pivotY: input?.pivotY ?? 0,
    margin: input?.margin ?? 0,
    alignment: input?.alignment ?? (root ? "fill" : "start"),
    sizeRule: input?.sizeRule ?? (root ? "fill" : "content"),
  };
}

function nodeSeedBase(element: UiElement, item: NodeDefinition): string {
  const explicit = item.props.id ?? item.props.stableId ??
    (element.key === undefined ? undefined : String(element.key)) ??
    (item.props.key === undefined ? undefined : String(item.props.key));
  return explicit ?? item.props.name ?? item.runtimeType ?? item.kind;
}

function siblingSeeds(children: readonly UiElement[]): readonly string[] {
  const occurrences = new Map<string, number>();
  return children.map((child) => {
    const source = definition(child);
    if (source.category !== "node") throw new TypeError("Scene children must be UI nodes");
    const base = nodeSeedBase(child, source);
    const occurrence = occurrences.get(base) ?? 0;
    occurrences.set(base, occurrence + 1);
    // Length-prefixing avoids path ambiguity when semantic identifiers contain `/` or `#`.
    return `${base.length}:${base}#${occurrence}`;
  });
}

function nodeAppearance(props: UiNodeProps): UiAppearance | undefined {
  if (props.appearance === undefined && props.style === undefined) return undefined;
  const style = props.style;
  return {
    ...props.appearance,
    ...(style?.backgroundColor === undefined && style?.background === undefined
      ? {} : {backgroundColor: style.backgroundColor ?? style.background}),
    ...(style?.textColor === undefined && style?.color === undefined
      ? {} : {textColor: style.textColor ?? style.color}),
    ...(style?.opacity === undefined ? {} : {opacity: style.opacity}),
    ...(style?.styleToken === undefined && style?.token === undefined
      ? {} : {styleToken: style.styleToken ?? style.token}),
    ...(style?.hoverBackgroundColor === undefined && style?.hoverBackground === undefined
      ? {} : {hoverBackgroundColor: style.hoverBackgroundColor ?? style.hoverBackground}),
    ...(style?.focusColor === undefined && style?.focus === undefined
      ? {} : {focusColor: style.focusColor ?? style.focus}),
    ...(style?.disabledOpacity === undefined ? {} : {disabledOpacity: style.disabledOpacity}),
  };
}

function nodeBindings(props: UiNodeProps): UiBindings {
  const shorthand = Object.fromEntries(Object.entries(props.bind ?? {}).map(([key, value]) =>
    [key, typeof value === "string" ? {path: value} : value]));
  return {...props.bindings, ...shorthand};
}

function lowerDocument(element: UiElement, path?: string): PrismatiXUiDocument {
  const rootDefinition = definition(element);
  if (rootDefinition.category !== "document") {
    throw new TypeError("PrismatiX JSX must export a Scene or Component root");
  }
  const props = rootDefinition.props;
  const authoredSeed = props.id ?? props.stableId ??
    (element.key === undefined ? undefined : String(element.key));
  const documentId = props.id !== undefined && uuidPattern.test(props.id)
    ? props.id.toLowerCase()
    : stableUuid("prismatix-ui-document",
      authoredSeed ?? (path === undefined ? props.name : `${path}\0${props.name}`));
  const roots = flattenChildren(props.children);
  if (roots.length !== 1) throw new TypeError("Scene and Component require exactly one root node");

  const nodes: PrismatiXUiNode[] = [];
  const visit = (item: UiElement, parentId: string | null, order: number,
    parentSeed: string, seed: string, containerOwned: boolean): string => {
    const source = definition(item);
    if (source.category !== "node") throw new TypeError("Scene children must be UI nodes");
    const id = source.props.id !== undefined && uuidPattern.test(source.props.id)
      ? source.props.id.toLowerCase()
      : stableUuid(documentId, `${parentSeed}/${seed}`);
    const children = flattenChildren(source.props.children);
    const structural = new Set<UiNodeKind>(["control", "stack", "hbox", "vbox", "grid", "group"]);
    if (children.length > 0 && !structural.has(source.kind)) {
      throw new TypeError(`${source.kind} nodes cannot contain child nodes`);
    }
    const root = parentId === null;
    const actionInput = source.props.onClick ?? source.props.action;
    const action = typeof actionInput === "string" ? {id: actionInput} : actionInput;
    const appearance = nodeAppearance(source.props);
    nodes.push({
      id, parentId, order, kind: source.kind,
      ...(source.runtimeType === undefined ? {} : {runtimeType: source.runtimeType}),
      name: source.props.name ?? source.runtimeType ?? source.kind,
      visible: source.props.visible ?? true,
      locked: source.props.locked ?? false,
      layout: nodeLayout(source.props, containerOwned, root, props.width, props.height),
      ...(source.props.text === undefined ? {} : {text: source.props.text}),
      ...(source.props.assetId === undefined ? {} : {assetId: source.props.assetId}),
      ...(appearance === undefined ? {} : {appearance}),
      ...(action === undefined ? {} : {onClick: {id: action.id, arguments: action.arguments ?? {}}}),
      ...(source.props.accessibilityLabel === undefined ? {} : {accessibilityLabel: source.props.accessibilityLabel}),
      ...(source.props.accessibilityRole === undefined ? {} : {accessibilityRole: source.props.accessibilityRole}),
      ...(source.props.accessibilityDescription === undefined ? {} : {accessibilityDescription: source.props.accessibilityDescription}),
      ...(source.props.accessibilityFocusOrder === undefined ? {} : {accessibilityFocusOrder: source.props.accessibilityFocusOrder}),
      runtimeProperties: source.props.runtimeProperties ?? {},
      bindings: nodeBindings(source.props),
      ...(source.props.componentInstance === undefined ? {} : {componentInstance: {
        ...source.props.componentInstance,
        overrides: source.props.componentInstance.overrides ?? [],
      }}),
      ...(source.props.componentSlot === undefined ? {} : {componentSlot: source.props.componentSlot}),
    });
    const ownsLayout = source.kind === "stack" || source.kind === "hbox" ||
      source.kind === "vbox" || source.kind === "grid";
    const seeds = siblingSeeds(children);
    children.forEach((child, index) => visit(child, id, index,
      `${parentSeed}/${seed}`, seeds[index]!, ownsLayout));
    return id;
  };
  const rootId = visit(roots[0]!, null, 0, "root", siblingSeeds(roots)[0]!, false);
  const common: PrismatiXUiDocumentBase = {
    schemaRevision: 2,
    id: documentId,
    revision: props.revision ?? 1,
    name: props.name,
    width: props.width,
    height: props.height,
    rootId,
    nodes,
    theme: props.theme ?? [],
  };
  if (rootDefinition.format === "PrismatiXUIComponent") {
    const component = props as ComponentProps;
    return {format: "PrismatiXUIComponent", ...common,
      componentInterface: component.componentInterface ?? {properties: [], signals: [], slots: []}};
  }
  const scene = props as SceneProps;
  return {
    format: "PrismatiXUIScene", ...common,
    behaviorGraph: scene.behaviorGraph ?? {nodes: [], links: [], groups: []},
    behaviorTriggers: scene.behaviorTriggers ?? [],
    visualStateGroups: scene.visualStateGroups ?? [],
    ...(scene.animations === undefined ? {} : {animations: scene.animations}),
  };
}

export interface CompileJsxUiOptions {
  readonly path?: string;
  readonly kind?: "scene" | "component";
}

/** Lowers a React-free JSX authoring tree into the existing canonical UI contract. */
export function compileJsxUi(
  element: UiElement,
  options: CompileJsxUiOptions = {},
): Result<PrismatiXUiDocument> {
  let value: PrismatiXUiDocument;
  try {
    value = lowerDocument(element, options.path);
  } catch (error) {
    return {valid: false, diagnostics: [diagnostic(
      "PXSDKJSX1001", "JSX UI could not be lowered", options.path, String(error),
    )]};
  }
  const actualKind = value.format === "PrismatiXUIScene" ? "scene" : "component";
  if (options.kind !== undefined && options.kind !== actualKind) {
    return {valid: false, diagnostics: [diagnostic(
      "PXSDKJSX1002", `Expected a JSX ${options.kind} document`, options.path, value.format,
    )]};
  }
  return validateDocument<PrismatiXUiDocument>(
    actualKind === "scene" ? "ui" : "uiComponent", value, options.path,
  );
}

/** Convenience form for SDK integrations that prefer exceptions over Result. */
export function renderJsxUi(element: UiElement, options: CompileJsxUiOptions = {}): PrismatiXUiDocument {
  const result = compileJsxUi(element, options);
  if (!result.valid || result.value === undefined) {
    throw new Error(result.diagnostics.map((value) =>
      `${value.code}: ${value.message}${value.details === undefined ? "" : ` (${value.details})`}`).join("\n"));
  }
  return result.value;
}
