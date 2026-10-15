import {createHash} from "node:crypto";

import {validateDocument} from "./contracts.js";
import {
  Fragment,
  isUiElement,
  uiElementMarker,
  uiJsxSourceMarker,
  uiLintMarker,
  type UiChild,
  type UiElement,
  type UiJsxSource,
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
  /** Box decoration, drawn behind the control without a texture. */
  readonly cornerRadius?: number;
  readonly borderWidth?: number;
  readonly borderColor?: string | null;
  readonly hoverBorderColor?: string | null;
  readonly hoverTextColor?: string | null;
  /** The background fades from `backgroundColor` to this colour. */
  readonly gradientColor?: string | null;
  readonly gradientDirection?: "vertical" | "horizontal";
  readonly shadowColor?: string | null;
  readonly shadowBlur?: number;
  readonly shadowOffsetX?: number;
  readonly shadowOffsetY?: number;
}

/** Friendly authoring aliases; these are lowered to canonical appearance fields. */
export interface UiStyle extends UiAppearance {
  readonly background?: string;
  readonly color?: string;
  readonly token?: string | null;
  readonly hoverBackground?: string | null;
  readonly focus?: string | null;
  /** `cornerRadius` */
  readonly radius?: number;
  /** `[width, color]`: `borderWidth` and `borderColor` */
  readonly border?: readonly [number, string];
  /** `[from, to]` or `[from, to, "horizontal"]`: `backgroundColor` fading to `gradientColor` */
  readonly gradient?: readonly [string, string] | readonly [string, string, "vertical" | "horizontal"];
  /** `{color, blur, x, y}`: the soft drop shadow */
  readonly shadow?: {readonly color: string; readonly blur?: number; readonly x?: number; readonly y?: number};
}

const boxAppearanceKeys = ["cornerRadius", "borderWidth", "borderColor", "hoverBorderColor",
  "hoverTextColor", "gradientColor", "gradientDirection", "shadowColor", "shadowBlur",
  "shadowOffsetX", "shadowOffsetY"] as const;

export interface UiBinding {
  readonly path: string;
  readonly formatter?: string;
}

export type UiBindings = Readonly<Record<string, UiBinding>>;

export interface UiAction {
  readonly id: string;
  readonly arguments?: Readonly<Record<string, JsonValue>>;
}

export interface UiItemSource {
  /** Typed ViewModel array. Template bindings use `item` as their root. */
  readonly path: string;
  /** Unique string or integer field within each item object. */
  readonly key: string;
  /** Per-item rectangle field for ItemsPlaced: {x, y, width, height}. */
  readonly placement?: string;
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
  /** Common native Control interaction properties. */
  readonly focusMode?: "None" | "Click" | "All";
  readonly mouseFilter?: "Stop" | "Pass" | "Ignore";
  /** Capture pointer navigation and keyboard focus while visible. */
  readonly modalCapture?: boolean;
  readonly hoverSoundPath?: string;
  readonly activateSoundPath?: string;
  /** Clip children to this control's rectangle. */
  readonly clipContent?: boolean;
  /** Fade (and optionally slide from `offset`) when the control becomes visible. */
  readonly showTransition?: UiVisibilityTransition;
  /** Fade (and optionally slide to `offset`) when the control is hidden. */
  readonly hideTransition?: UiVisibilityTransition;
  /** Use linear instead of eased show/hide transitions. */
  readonly linearTransition?: boolean;
  readonly bindings?: UiBindings;
  /** Shorthand bindings; a `"!path"` string applies the `Not` formatter. */
  readonly bind?: Readonly<Record<string, string | UiBinding>>;
  /** Shows the current locale's `strings[textKey]`, following language changes. */
  readonly textKey?: string;
  /** Visible only while the UI data path is true; `"!path"` negates it. */
  readonly visibleWhen?: string;
  /** Enabled only while the UI data path is true; `"!path"` negates it. */
  readonly enabledWhen?: string;
  readonly onClick?: UiAction;
  readonly action?: string | UiAction;
  readonly accessibilityLabel?: string;
  readonly accessibilityRole?: string;
  readonly accessibilityDescription?: string;
  readonly accessibilityFocusOrder?: number;
  readonly componentInstance?: UiComponentInstance;
  readonly componentSlot?: UiComponentSlot;
  readonly itemSource?: UiItemSource;
  readonly children?: UiChild;
}

export interface UiVisibilityTransition {
  readonly seconds: number;
  /** Pixels the control slides from (show) or to (hide). */
  readonly offset?: readonly [x: number, y: number];
}

export interface ItemsViewProps extends UiNodeProps {
  readonly items: UiItemSource;
  readonly columns?: number;
  readonly itemExtent?: readonly [width: number, height: number];
  readonly gap?: readonly [x: number, y: number];
  readonly overscan?: number;
}

export interface ItemsPlacedProps extends Omit<UiNodeProps, "itemSource"> {
  readonly items: UiItemSource & {readonly placement: string};
  /**
   * Where the Player puts story choices when this view lists
   * `dialogue.choices`: `area` is the middle choice bar `[x, y, width, height]`
   * in this view's space, and each choice moves down by `pitch` times its
   * offset. `rows` gives the offsets for a choice count, e.g.
   * `{2: [-1, 1], 3: [-1.5, 0, 1.5]}`; other counts sit one pitch apart.
   */
  readonly choiceLayout?: {
    readonly area: readonly [x: number, y: number, width: number, height: number];
    readonly pitch?: number;
    readonly rows?: Readonly<Record<number, readonly number[]>>;
  };
}

export interface ControlProps extends UiNodeProps {
  readonly kind?: UiNodeKind;
  readonly runtimeType?: string;
  /** Caption of a native control that shows one, such as a CheckBox. */
  readonly text?: string;
}

export interface ModalProps extends Omit<UiNodeProps, "children"> {
  /** The panel receives `children` in panel-local coordinates. */
  readonly panel: Readonly<{x: number; y: number; width: number; height: number}>;
  readonly children?: UiChild;
  /** Optional authored scrim image. Without one, a translucent color is used. */
  readonly backdropPath?: string;
  readonly backdropColor?: string;
  readonly backdropAction?: string | UiAction;
  readonly backdropLabel?: string;
}

export interface TextProps extends Omit<UiNodeProps, "children"> {
  readonly text?: string;
  readonly fontSize?: number;
  /** Project font file for this label; the theme font is used otherwise. */
  readonly font?: string;
  /** Extra pixels between the characters of a single-line label. */
  readonly letterSpacing?: number;
  readonly wrap?: boolean;
  readonly outlineSize?: number;
  readonly outlineColor?: string;
  readonly horizontalAlignment?: "Left" | "Center" | "Right";
  readonly verticalAlignment?: "Top" | "Center" | "Bottom";
  readonly children?: string | number | readonly (string | number)[];
}

export interface ColorRectProps extends UiNodeProps {
  /** #RRGGBB or #RRGGBBAA. */
  readonly color: string;
}

export interface ImageProps extends UiNodeProps {
  readonly assetId?: string | null;
  readonly path?: string;
  readonly scaleMode?: "Stretch" | "Fit" | "Fill" | "Original";
  readonly lockAspectRatio?: boolean;
  readonly opacity?: number;
  /** Atlas cell `[x, y, width, height]` in image pixels; placed like a whole image of that size. */
  readonly region?: readonly [x: number, y: number, width: number, height: number];
}

/** A layered, time-sampled .pxmotion clip rendered inside a UI layout box. */
export interface MotionProps extends UiNodeProps {
  readonly path: string;
  readonly autoplay?: boolean;
  readonly playing?: boolean;
  readonly speed?: number;
  /** Playback time in seconds; `position` remains the layout coordinate prop. */
  readonly playhead?: number;
}

export interface SpriteGlyph {
  readonly character: string;
  readonly path: string;
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
}

/** Data-driven sprite lettering without a hidden image node for every glyph. */
export interface SpriteTextProps extends Omit<UiNodeProps, "children"> {
  readonly text?: string;
  readonly glyphs: readonly SpriteGlyph[];
  readonly advance?: number;
}

export interface ButtonProps extends Omit<UiNodeProps, "children"> {
  readonly text?: string;
  readonly fontSize?: number;
  readonly horizontalAlignment?: "Left" | "Center" | "Right";
  readonly verticalAlignment?: "Top" | "Center" | "Bottom";
  readonly children?: string | number | readonly (string | number)[];
}

/** Image states named `${base}-normal.png`, `${base}-hover.png`, ... */
export interface TextureButtonSkin {
  readonly base: string;
  /** States with their own image; pressed falls back to hover. Default: hover. */
  readonly states?: readonly ("hover" | "pressed" | "disabled")[];
  /** File extension including the dot. Default: `.png`. */
  readonly extension?: string;
}

export interface TextureButtonProps extends ButtonProps {
  /**
   * Derives state images from one path stem, for example
   * `skin="Assets/UI/save"` uses `save-normal.png` and `save-hover.png`
   * (also for pressed). Explicit `*Path` props win.
   */
  readonly skin?: string | TextureButtonSkin;
  readonly normalPath?: string;
  readonly hoverPath?: string;
  readonly pressedPath?: string;
  readonly disabledPath?: string;
  readonly scaleMode?: "Stretch" | "Fit" | "Fill" | "Original";
  /**
   * Shown while the pointer is over this button, positioned relative to the
   * button. The button is wrapped in a `<name>Group` and needs a `name`.
   */
  readonly tooltip?: UiChild;
}

type StateButtonBase = Omit<TextureButtonProps, "skin" | "action" | "onClick" | "normalPath" |
  "hoverPath" | "pressedPath" | "disabledPath" | "tooltip" | "children" | "text">;

/**
 * Two-state image button. Either `option` names a `player.options` entry
 * (on = `onValue`, default 1; off = `offValue`, default 0) or `checked` names
 * a boolean UI data path and `action` flips it. Lowers to `<name>On` and
 * `<name>Off` buttons inside a `<name>` group.
 */
export interface ToggleProps extends StateButtonBase {
  readonly option?: string;
  readonly onValue?: number;
  readonly offValue?: number;
  readonly checked?: string;
  readonly action?: string | UiAction;
  /** Skin shown while on. */
  readonly on: string | TextureButtonSkin;
  /** Skin shown while off. */
  readonly off: string | TextureButtonSkin;
}

/** One choice of a `player.options` entry; `selected` is shown while it is chosen. */
export interface RadioProps extends StateButtonBase {
  readonly option: string;
  readonly value: number;
  readonly skin: string | TextureButtonSkin;
  readonly selected: string | TextureButtonSkin;
}

function optionAction(option: string, value: number): UiAction {
  return {id: "option.set", arguments: {argument: `${option}=${value}`}};
}

function stateButtons(props: StateButtonBase, role: string, states: readonly {
  readonly suffix: string; readonly skin: string | TextureButtonSkin;
  readonly visibleWhen: string; readonly action?: string | UiAction; readonly label?: string;
}[]): UiElement {
  if (props.name === undefined) throw new TypeError(`A ${role} needs a name`);
  const box = Object.fromEntries(layoutProps.filter((key) => props[key] !== undefined)
    .map((key) => [key, props[key]]));
  const inner = {...props} as Record<string, unknown>;
  for (const key of layoutProps) delete inner[key];
  return Group({...box, name: props.name, mouseFilter: "Ignore",
    ...(props.visibleWhen === undefined ? {} : {visibleWhen: props.visibleWhen}),
    children: states.map((state) => TextureButton({
      ...inner, name: `${props.name}${state.suffix}`, fill: true, skin: state.skin,
      visibleWhen: state.visibleWhen, accessibilityRole: props.accessibilityRole ?? role,
      ...(state.label === undefined ? {} : {accessibilityLabel: state.label}),
      ...(state.action === undefined ? {} : {action: state.action}),
    } as unknown as TextureButtonProps))});
}

export function Toggle(props: ToggleProps): UiElement {
  const {option, onValue = 1, offValue = 0, checked, action, on, off, ...rest} = props;
  if ((option === undefined) === (checked === undefined))
    throw new TypeError(`Toggle ${props.name ?? ""} needs exactly one of option or checked`);
  if (checked !== undefined && action === undefined)
    throw new TypeError(`Toggle ${props.name ?? ""} with checked needs the action that flips it`);
  const isOn = option !== undefined ? `options.${option}.is${onValue}` : checked!;
  return stateButtons(rest, "checkbox", [
    {suffix: "Off", skin: off, visibleWhen: `!${isOn}`,
      action: option !== undefined ? optionAction(option, onValue) : action!},
    {suffix: "On", skin: on, visibleWhen: isOn,
      action: option !== undefined ? optionAction(option, offValue) : action!},
  ]);
}

export function Radio(props: RadioProps): UiElement {
  const {option, value, skin, selected, ...rest} = props;
  const chosen = `options.${option}.is${value}`;
  return stateButtons(rest, "radio", [
    {suffix: "Button", skin, visibleWhen: `!${chosen}`, action: optionAction(option, value)},
    {suffix: "Selected", skin: selected, visibleWhen: chosen},
  ]);
}

/** Native numeric range input with optional image rail and knob. */
export interface SliderProps extends Omit<UiNodeProps, "children"> {
  readonly minimum?: number;
  readonly maximum?: number;
  readonly step?: number;
  readonly value?: number;
  readonly railPath?: string;
  readonly railHoverPath?: string;
  readonly railPressedPath?: string;
  readonly knobPath?: string;
  readonly knobHoverPath?: string;
  readonly knobPressedPath?: string;
  readonly railHeight?: number;
  readonly knobSize?: number;
  /** Clip the active rail image to the current value, preserving its full source scale. */
  readonly fillRailToValue?: boolean;
  /** The `player.options` entry this slider shows and edits. */
  readonly option?: string;
}

/** A skinnable button that displays and edits one registered keyboard action. */
export interface KeyBindingButtonProps extends Omit<TextureButtonProps,
  "onClick" | "action" | "text" | "children"> {
  readonly bindingAction: string;
  readonly bindingPrompt?: string;
  readonly unboundText?: string;
  readonly bindingConflictText?: string;
  readonly bindingConfirmText?: string;
  readonly bindingLimitText?: string;
  readonly escapeCancelsBinding?: boolean;
  readonly allowSharedBinding?: boolean;
  /** Ask for a second activation before intentionally sharing a conflicting key. */
  readonly confirmSharedBinding?: boolean;
  /** Delete during capture clears only keyboard keys, preserving gamepad/wheel. */
  readonly deleteClearsBinding?: boolean;
  /** Add the captured key instead of replacing all current keyboard keys. */
  readonly addCapturedBinding?: boolean;
  /** Select a displayed key with Page Up/Down or the wheel, then replace or delete it. */
  readonly selectableBindingKeys?: boolean;
}

/** A skinnable button that captures a physical scroll direction for one action. */
export interface WheelBindingButtonProps extends Omit<TextureButtonProps,
  "onClick" | "action" | "text" | "children"> {
  readonly bindingAction: string;
  readonly bindingPrompt?: string;
  readonly unboundText?: string;
  readonly wheelUpText?: string;
  readonly wheelDownText?: string;
  /** Action activation threshold, from 0 to 100; zero accepts any movement. */
  readonly wheelThreshold?: number;
  readonly escapeCancelsBinding?: boolean;
  /** Delete during capture clears only the wheel binding. */
  readonly deleteClearsBinding?: boolean;
}

export interface RichTextLabelProps extends TextProps {
  /** Ruby/markup source; takes precedence over text children. */
  readonly markup?: string;
  readonly vertical?: boolean;
  readonly verticalRows?: number;
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
  readonly itemSource?: UiItemSource;
}

interface PrismatiXUiDocumentBase {
  readonly schemaRevision: 1;
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
const structuralNodeKinds = new Set<UiNodeKind>(["control", "stack", "hbox", "vbox", "grid", "group"]);

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
  const {focusMode, mouseFilter, modalCapture, hoverSoundPath, activateSoundPath,
    clipContent, showTransition, hideTransition, linearTransition, ...rest} = props;
  const transition = (phase: "show" | "hide", value: UiVisibilityTransition | undefined) =>
    value === undefined ? {} : {
      [`${phase}TransitionSeconds`]: value.seconds,
      ...(value.offset === undefined ? {}
        : {[`${phase}OffsetX`]: value.offset[0], [`${phase}OffsetY`]: value.offset[1]}),
    };
  return definitionElement(nodeType, {
    category: "node", kind,
    ...(runtimeType === undefined ? {} : {runtimeType}),
    props: {...rest, runtimeProperties: {
      ...props.runtimeProperties,
      ...(focusMode === undefined ? {} : {focusMode}),
      ...(mouseFilter === undefined ? {} : {mouseFilter}),
      ...(modalCapture === undefined ? {} : {modalCapture}),
      ...(hoverSoundPath === undefined ? {} : {hoverSoundPath}),
      ...(activateSoundPath === undefined ? {} : {activateSoundPath}),
      ...(clipContent === undefined ? {} : {clipContent}),
      ...transition("show", showTransition),
      ...transition("hide", hideTransition),
      ...(linearTransition === undefined ? {} : {linearTransition}),
    }},
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

/** Backdrop, blocking panel, and native modal focus/capture as one authored node. */
export function Modal(props: ModalProps): UiElement {
  const {panel, children, backdropPath, backdropColor, backdropAction,
    backdropLabel, ...rest} = props;
  const backdrop = backdropPath === undefined
    ? Button({name: "ModalBackdrop", fill: true, text: "", focusMode: "None",
      style: {background: backdropColor ?? "#00000099"},
      ...(backdropAction === undefined ? {} : {action: backdropAction}),
      ...(backdropLabel === undefined ? {} : {accessibilityLabel: backdropLabel})})
    : TextureButton({name: "ModalBackdrop", fill: true, focusMode: "None",
      normalPath: backdropPath, hoverPath: backdropPath, pressedPath: backdropPath,
      scaleMode: "Stretch",
      ...(backdropAction === undefined ? {} : {action: backdropAction}),
      ...(backdropLabel === undefined ? {} : {accessibilityLabel: backdropLabel})});
  const content = Control({name: "ModalPanel", ...panel, mouseFilter: "Stop", children});
  return node("control", "Control", {...rest, modalCapture: true,
    accessibilityRole: props.accessibilityRole ?? "dialog",
    children: [backdrop, content]});
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

/** Runtime-keyed grid: author exactly one item subtree as `children`. */
export function ItemsGrid(props: ItemsViewProps): UiElement {
  const {items, columns, itemExtent, gap, overscan, ...rest} = props;
  return node("grid", "GridView", {...rest, itemSource: items,
    runtimeProperties: {...props.runtimeProperties,
      ...(columns === undefined ? {} : {columns}),
      ...(itemExtent === undefined ? {} : {itemExtent: {type: "vec2", x: itemExtent[0], y: itemExtent[1]}}),
      ...(gap === undefined ? {} : {gap: {type: "vec2", x: gap[0], y: gap[1]}}),
      ...(overscan === undefined ? {} : {overscan}),
    }});
}

/** Runtime-keyed vertical list with one authored item subtree. */
export function ItemsList(props: Omit<ItemsViewProps, "columns">): UiElement {
  const {items, itemExtent, gap, overscan, ...rest} = props;
  return node("control", "ListView", {...rest, itemSource: items,
    runtimeProperties: {...props.runtimeProperties,
      ...(itemExtent === undefined ? {} : {itemExtent: {type: "vec2", x: itemExtent[0], y: itemExtent[1]}}),
      ...(gap === undefined ? {} : {gap: {type: "vec2", x: gap[0], y: gap[1]}}),
      ...(overscan === undefined ? {} : {overscan}),
    }});
}

export interface TextFieldProps extends UiNodeProps {
  /** Initial text; a Story [input] replaces it with its default. */
  readonly text?: string;
  /** Hint shown while the field is empty. */
  readonly placeholder?: string;
}

/**
 * A single-line text field. Name it `TextInput` in the HUD and a Story
 * `[input var=name]` uses it to ask the player for text; Enter or an
 * `input.accept` button stores the text.
 */
export function TextField(props: TextFieldProps): UiElement {
  const {text, placeholder, ...rest} = props;
  return node("control", "LineEdit", {...rest, runtimeProperties: {
    ...props.runtimeProperties,
    ...(text === undefined ? {} : {text}),
    ...(placeholder === undefined ? {} : {placeholder}),
  }});
}

/** Runtime-keyed items at data-authored local rectangles; one item subtree. */
export function ItemsPlaced(props: ItemsPlacedProps): UiElement {
  const {items, choiceLayout, ...rest} = props;
  if (choiceLayout === undefined)
    return node("control", "PlacedItemsView", {...rest, itemSource: items});
  const {area, pitch = 0, rows = {}} = choiceLayout;
  const label = `ItemsPlaced ${props.name ?? ""} choiceLayout`;
  if (area.length !== 4 || area.some((value) => !Number.isFinite(value)) || area[2] <= 0 || area[3] <= 0)
    throw new TypeError(`${label} area needs [x, y, width, height] with a positive size`);
  if (!Number.isFinite(pitch)) throw new TypeError(`${label} pitch must be a finite number`);
  const encoded = Object.entries(rows).map(([count, offsets]) => {
    if (!/^[1-9][0-9]?$/u.test(count) || Number(count) > 64 || offsets.length !== Number(count) ||
        offsets.some((offset) => !Number.isFinite(offset)))
      throw new TypeError(`${label} row ${count} needs ${count} finite offsets`);
    return `${count}:${offsets.join(",")}`;
  }).join(";");
  return node("control", "PlacedItemsView", {...rest, itemSource: items, runtimeProperties: {
    ...props.runtimeProperties,
    choiceArea: {type: "rect", x: area[0], y: area[1], width: area[2], height: area[3]},
    choicePitch: pitch,
    ...(encoded === "" ? {} : {choiceRows: encoded}),
  }});
}

export function Text(props: TextProps): UiElement {
  const text = props.text ?? textualChildren(props.children);
  const {children: _children, fontSize, font, letterSpacing, wrap, outlineSize, outlineColor,
    horizontalAlignment, verticalAlignment, ...rest} = props;
  return node("label", "Label", {...rest, ...(text === undefined ? {} : {text}),
    runtimeProperties: {...props.runtimeProperties,
      ...(fontSize === undefined ? {} : {fontSize}),
      ...(font === undefined ? {} : {fontPath: font}),
      ...(letterSpacing === undefined ? {} : {letterSpacing}),
      ...(wrap === undefined ? {} : {wrap}),
      ...(outlineSize === undefined ? {} : {outlineSize}),
      ...(outlineColor === undefined ? {} : {outlineColor: {type: "color", value: outlineColor}}),
      ...(horizontalAlignment === undefined ? {} : {horizontalAlignment}),
      ...(verticalAlignment === undefined ? {} : {verticalAlignment})}});
}

/** A solid rectangle: backgrounds, scrims, separators. Ignores the mouse by default. */
export function ColorRect(props: ColorRectProps): UiElement {
  const {color, ...rest} = props;
  return node("control", "ColorRect", {...rest, mouseFilter: rest.mouseFilter ?? "Ignore",
    runtimeProperties: {...props.runtimeProperties, color: {type: "color", value: color}}});
}

export function Image(props: ImageProps): UiElement {
  const {path, scaleMode, lockAspectRatio, opacity, region, ...rest} = props;
  if (region !== undefined && region.some((value) => !Number.isFinite(value) || value < 0))
    throw new TypeError(`Image ${props.name ?? ""} region needs four non-negative numbers`);
  return node("image", "TextureRect", {...rest, runtimeProperties: {
    ...props.runtimeProperties,
    ...(path === undefined ? {} : {path}),
    ...(scaleMode === undefined ? {} : {scaleMode}),
    ...(lockAspectRatio === undefined ? {} : {lockAspectRatio}),
    ...(opacity === undefined ? {} : {opacity}),
    ...(region === undefined ? {} : {region: {type: "rect", x: region[0], y: region[1],
      width: region[2], height: region[3]}}),
  }});
}

export function Motion(props: MotionProps): UiElement {
  const {path, autoplay, playing, speed, playhead, ...rest} = props;
  return node("image", "MotionRect", {...rest, runtimeProperties: {
    ...props.runtimeProperties,
    path,
    ...(autoplay === undefined ? {} : {autoplay}),
    ...(playing === undefined ? {} : {playing}),
    ...(speed === undefined ? {} : {speed}),
    ...(playhead === undefined ? {} : {position: playhead}),
  }});
}

export function SpriteText(props: SpriteTextProps): UiElement {
  const {text, glyphs, advance, ...rest} = props;
  return node("image", "SpriteText", {...rest, runtimeProperties: {
    ...props.runtimeProperties,
    glyphs: glyphs.map((glyph) => ({...glyph})),
    ...(text === undefined ? {} : {text}),
    ...(advance === undefined ? {} : {advance}),
  }});
}

function buttonNode(runtimeType: "Button" | "TextureButton", props: ButtonProps): UiElement {
  const text = props.text ?? textualChildren(props.children);
  const {children: _children, fontSize, horizontalAlignment, verticalAlignment, ...rest} = props;
  return node("button", runtimeType, {...rest, ...(text === undefined ? {} : {text}),
    runtimeProperties: {...props.runtimeProperties,
      ...(fontSize === undefined ? {} : {fontSize}),
      ...(horizontalAlignment === undefined ? {} : {horizontalAlignment}),
      ...(verticalAlignment === undefined ? {} : {verticalAlignment})}});
}

export function Button(props: ButtonProps): UiElement {
  return buttonNode("Button", props);
}

function skinPaths(skin: TextureButtonProps["skin"]): {
  normalPath?: string; hoverPath?: string; pressedPath?: string; disabledPath?: string;
} {
  if (skin === undefined) return {};
  const {base, states = ["hover"], extension = ".png"} =
    typeof skin === "string" ? {base: skin} : skin;
  const state = (name: string) => `${base}-${name}${extension}`;
  const hover = states.includes("hover") ? state("hover") : undefined;
  const pressed = states.includes("pressed") ? state("pressed") : hover;
  return {
    normalPath: state("normal"),
    ...(hover === undefined ? {} : {hoverPath: hover}),
    ...(pressed === undefined ? {} : {pressedPath: pressed}),
    ...(states.includes("disabled") ? {disabledPath: state("disabled")} : {}),
  };
}

const layoutProps = ["layout", "position", "size", "x", "y", "width", "height", "margin",
  "align", "fill", "anchor", "pivot"] as const;

/** Image-state button without the generic Control runtimeType escape hatch. */
export function TextureButton(props: TextureButtonProps): UiElement {
  if (props.tooltip !== undefined) {
    // The Player publishes hover.<name> while the pointer is over a named
    // button; the tooltip rides beside the button inside one layout box.
    const {tooltip, ...button} = props;
    if (button.name === undefined) throw new TypeError("A TextureButton with a tooltip needs a name");
    const box = Object.fromEntries(layoutProps.filter((key) => button[key] !== undefined)
      .map((key) => [key, button[key]]));
    const inner = {...button} as Record<string, unknown>;
    for (const key of layoutProps) delete inner[key];
    return Group({...box, name: `${button.name}Group`, mouseFilter: "Ignore",
      ...(button.visibleWhen === undefined ? {} : {visibleWhen: button.visibleWhen}),
      children: [
        TextureButton({...inner, fill: true, visibleWhen: undefined} as unknown as TextureButtonProps),
        Group({name: `${button.name}Tooltip`, fill: true, mouseFilter: "Ignore",
          bind: {visible: `hover.${button.name}`}, children: tooltip}),
      ]});
  }
  const text = props.text ?? textualChildren(props.children);
  const {children: _children, skin, scaleMode, ...rest} = props;
  const derived = skinPaths(skin);
  const normalPath = props.normalPath ?? derived.normalPath;
  const hoverPath = props.hoverPath ?? derived.hoverPath;
  const pressedPath = props.pressedPath ?? derived.pressedPath;
  const disabledPath = props.disabledPath ?? derived.disabledPath;
  delete (rest as {normalPath?: string}).normalPath;
  delete (rest as {hoverPath?: string}).hoverPath;
  delete (rest as {pressedPath?: string}).pressedPath;
  delete (rest as {disabledPath?: string}).disabledPath;
  return buttonNode("TextureButton", {
    ...rest,
    ...(text === undefined ? {} : {text}),
    runtimeProperties: {
      ...props.runtimeProperties,
      ...(normalPath === undefined ? {} : {normalPath}),
      ...(hoverPath === undefined ? {} : {hoverPath}),
      ...(pressedPath === undefined ? {} : {pressedPath}),
      ...(disabledPath === undefined ? {} : {disabledPath}),
      ...(scaleMode === undefined ? {} : {scaleMode}),
    },
  });
}

/** Typed native Slider; replaces `<Control runtimeType="Slider">`. */
export function Slider(props: SliderProps): UiElement {
  const {minimum, maximum, step, value, railPath, railHoverPath,
    railPressedPath, knobPath, knobHoverPath, knobPressedPath, railHeight,
    knobSize, fillRailToValue, option, ...rest} = props;
  const optional = {minimum, maximum, step, value, railPath, railHoverPath,
    railPressedPath, knobPath, knobHoverPath, knobPressedPath, railHeight,
    knobSize, fillRailToValue, optionKey: option};
  return node("control", "Slider", {
    ...rest,
    accessibilityRole: props.accessibilityRole ?? "slider",
    runtimeProperties: {
      ...props.runtimeProperties,
      ...Object.fromEntries(Object.entries(optional)
        .filter((entry): entry is [string, string | number | boolean] => entry[1] !== undefined)),
    },
  });
}

/** Click to capture a physical key; the host persists successful changes. */
export function KeyBindingButton(props: KeyBindingButtonProps): UiElement {
  const {bindingAction, bindingPrompt, unboundText, bindingConflictText,
    bindingConfirmText, bindingLimitText, escapeCancelsBinding,
    allowSharedBinding, confirmSharedBinding,
    deleteClearsBinding, addCapturedBinding, selectableBindingKeys,
    ...rest} = props;
  return TextureButton({...rest, runtimeProperties: {
    ...props.runtimeProperties,
    bindingAction,
    ...(bindingPrompt === undefined ? {} : {bindingPrompt}),
    ...(unboundText === undefined ? {} : {unboundText}),
    ...(bindingConflictText === undefined ? {} : {bindingConflictText}),
    ...(bindingConfirmText === undefined ? {} : {bindingConfirmText}),
    ...(bindingLimitText === undefined ? {} : {bindingLimitText}),
    ...(escapeCancelsBinding === undefined ? {} : {escapeCancelsBinding}),
    ...(allowSharedBinding === undefined ? {} : {allowSharedBinding}),
    ...(confirmSharedBinding === undefined ? {} : {confirmSharedBinding}),
    ...(deleteClearsBinding === undefined ? {} : {deleteClearsBinding}),
    ...(addCapturedBinding === undefined ? {} : {addCapturedBinding}),
    ...(selectableBindingKeys === undefined ? {} : {selectableBindingKeys}),
  }});
}

/** Click to capture wheel up/down; the host persists successful changes. */
export function WheelBindingButton(props: WheelBindingButtonProps): UiElement {
  const {bindingAction, bindingPrompt, unboundText, wheelUpText,
    wheelDownText, wheelThreshold, escapeCancelsBinding,
    deleteClearsBinding, ...rest} = props;
  return TextureButton({...rest, runtimeProperties: {
    ...props.runtimeProperties,
    bindingAction,
    bindingDevice: "wheel",
    bindingPrompt: bindingPrompt ?? "Scroll up or down",
    ...(unboundText === undefined ? {} : {unboundText}),
    ...(wheelUpText === undefined ? {} : {wheelUpText}),
    ...(wheelDownText === undefined ? {} : {wheelDownText}),
    ...(wheelThreshold === undefined ? {} : {wheelThreshold}),
    ...(escapeCancelsBinding === undefined ? {} : {escapeCancelsBinding}),
    ...(deleteClearsBinding === undefined ? {} : {deleteClearsBinding}),
  }});
}

/** Rich text and ruby markup rendered by the native RichTextLabel control. */
export function RichTextLabel(props: RichTextLabelProps): UiElement {
  const text = props.markup ?? props.text ?? textualChildren(props.children);
  const {children: _children, markup: _markup, vertical, verticalRows,
    fontSize, wrap, outlineSize, outlineColor, horizontalAlignment, verticalAlignment,
    ...rest} = props;
  return node("label", "RichTextLabel", {
    ...rest,
    ...(text === undefined ? {} : {text}),
    runtimeProperties: {
      ...props.runtimeProperties,
      ...(fontSize === undefined ? {} : {fontSize}),
      ...(wrap === undefined ? {} : {wrap}),
      ...(outlineSize === undefined ? {} : {outlineSize}),
      ...(outlineColor === undefined ? {} : {outlineColor: {type: "color", value: outlineColor}}),
      ...(horizontalAlignment === undefined ? {} : {horizontalAlignment}),
      ...(verticalAlignment === undefined ? {} : {verticalAlignment}),
      ...(vertical === undefined ? {} : {vertical}),
      ...(verticalRows === undefined ? {} : {verticalRows}),
    },
  });
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
  parentWidth: number,
  parentHeight: number,
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
    width: input?.width ?? (root || input?.sizeRule === "fill" ||
      (input?.anchorRight ?? 0) > (input?.anchorX ?? 0) ? parentWidth : 1),
    height: input?.height ?? (root || input?.sizeRule === "fill" ||
      (input?.anchorBottom ?? 0) > (input?.anchorY ?? 0) ? parentHeight : 1),
    anchorX: input?.anchorX ?? 0,
    anchorY: input?.anchorY ?? 0,
    anchorRight: input?.anchorRight ?? (root ? 1 : 0),
    anchorBottom: input?.anchorBottom ?? (root ? 1 : 0),
    pivotX: input?.pivotX ?? 0,
    pivotY: input?.pivotY ?? 0,
    margin: input?.margin ?? 0,
    alignment: input?.alignment ?? (root ? "fill" : "start"),
    // An explicit rectangle in free layout is exact. Leaving it as content
    // lets the native control's intrinsic measure silently enlarge it (for
    // example a 34px texture button becomes 88px wide).
    sizeRule: input?.sizeRule ?? (root ? "fill" :
      !containerOwned && input.width !== undefined && input.height !== undefined
        ? "fixed" : "content"),
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
    ...Object.fromEntries(boxAppearanceKeys.filter((key) => style?.[key] !== undefined)
      .map((key) => [key, style![key]])),
    ...(style?.radius === undefined ? {} : {cornerRadius: style.radius}),
    ...(style?.border === undefined ? {} : {borderWidth: style.border[0], borderColor: style.border[1]}),
    ...(style?.gradient === undefined ? {} : {
      ...(style.backgroundColor === undefined && style.background === undefined
        ? {backgroundColor: style.gradient[0]} : {}),
      gradientColor: style.gradient[1],
      ...(style.gradient[2] === undefined ? {} : {gradientDirection: style.gradient[2]})}),
    ...(style?.shadow === undefined ? {} : {
      shadowColor: style.shadow.color, shadowBlur: style.shadow.blur ?? 12,
      shadowOffsetX: style.shadow.x ?? 0, shadowOffsetY: style.shadow.y ?? 4}),
  };
}

function nodeBindings(props: UiNodeProps): UiBindings {
  // `bind={{enabled: "!saveLoad.page1"}}` is the Not-formatted binding.
  const parse = (value: string | UiBinding): UiBinding => typeof value !== "string" ? value
    : value.startsWith("!") ? {path: value.slice(1), formatter: "Not"} : {path: value};
  const shorthand = Object.fromEntries(Object.entries(props.bind ?? {}).map(([key, value]) =>
    [key, parse(value)]));
  // visibleWhen/enabledWhen are separate props so a helper can pass either
  // without one `bind` object replacing the other.
  const conditions: Record<string, UiBinding> = {};
  for (const [property, value] of [["visible", props.visibleWhen], ["enabled", props.enabledWhen]] as const) {
    if (value === undefined) continue;
    if (props.bind?.[property] !== undefined || props.bindings?.[property] !== undefined)
      throw new TypeError(`${property}When and a ${property} binding cannot both be set on ${props.name ?? "a node"}`);
    conditions[property] = parse(value);
  }
  if (props.textKey !== undefined) {
    if (props.bind?.text !== undefined || props.bindings?.text !== undefined)
      throw new TypeError(`textKey and a text binding cannot both be set on ${props.name ?? "a node"}`);
    conditions.text = {path: `strings.${props.textKey}`};
  }
  return {...props.bindings, ...shorthand, ...conditions};
}

function lowerDocument(element: UiElement, path?: string,
  onNodeSource?: (index: number, source: UiJsxSource) => void,
  lints: AuthoringDiagnostic[] = []): PrismatiXUiDocument {
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
  if (roots.length === 0) throw new TypeError("Scene and Component require at least one UI node");
  // The wire contract has one structural root. Keep an authored structural
  // root untouched, but let simple scenes and sibling elements omit boilerplate.
  const rootSource = roots.length === 1 ? definition(roots[0]!) : undefined;
  const authoredRoot = roots.length === 1 && rootSource?.category === "node" &&
    structuralNodeKinds.has(rootSource.kind)
    ? roots[0]!
    : Group({name: "Root", stableId: "__prismatix_implicit_root__", fill: true, children: roots});

  const nodes: PrismatiXUiNode[] = [];
  const visit = (item: UiElement, parentId: string | null, order: number,
    parentSeed: string, seed: string, containerOwned: boolean,
    parentWidth: number, parentHeight: number): string => {
    const source = definition(item);
    if (source.category !== "node") throw new TypeError("Scene children must be UI nodes");
    const id = source.props.id !== undefined && uuidPattern.test(source.props.id)
      ? source.props.id.toLowerCase()
      : stableUuid(documentId, `${parentSeed}/${seed}`);
    const children = flattenChildren(source.props.children);
    if (children.length > 0 && !structuralNodeKinds.has(source.kind)) {
      throw new TypeError(`${source.kind} nodes cannot contain child nodes`);
    }
    const root = parentId === null;
    const actionInput = source.props.onClick ?? source.props.action;
    const action = typeof actionInput === "string" ? {id: actionInput} : actionInput;
    const appearance = nodeAppearance(source.props);
    const layout = nodeLayout(source.props, containerOwned, root,
      parentWidth, parentHeight);
    const jsxSource = item[uiJsxSourceMarker];
    if (jsxSource !== undefined) onNodeSource?.(nodes.length, jsxSource);
    for (const lint of item[uiLintMarker] ?? [])
      lints.push({severity: "warning", code: lint.code, message: lint.message,
        ...(path === undefined ? {} : {path}),
        details: `nodes[${nodes.length}].${lint.property}`});
    nodes.push({
      id, parentId, order, kind: source.kind,
      ...(source.runtimeType === undefined ? {} : {runtimeType: source.runtimeType}),
      name: source.props.name ?? source.runtimeType ?? source.kind,
      visible: source.props.visible ?? true,
      locked: source.props.locked ?? false,
      layout,
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
      ...(source.props.itemSource === undefined ? {} : {itemSource: source.props.itemSource}),
    });
    const ownsLayout = source.kind === "stack" || source.kind === "hbox" ||
      source.kind === "vbox" || source.kind === "grid";
    const seeds = siblingSeeds(children);
    children.forEach((child, index) => visit(child, id, index,
      `${parentSeed}/${seed}`, seeds[index]!, ownsLayout,
      layout.width, layout.height));
    return id;
  };
  const rootId = visit(authoredRoot, null, 0, "root",
    siblingSeeds([authoredRoot])[0]!, false, props.width, props.height);
  const common: PrismatiXUiDocumentBase = {
    schemaRevision: 1,
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
  /** Reports authored JSX call sites for canonical node indices. */
  readonly onNodeSource?: (index: number, source: UiJsxSource) => void;
}

/** Lowers a React-free JSX authoring tree into the existing canonical UI contract. */
export function compileJsxUi(
  element: UiElement,
  options: CompileJsxUiOptions = {},
): Result<PrismatiXUiDocument> {
  let value: PrismatiXUiDocument;
  const lints: AuthoringDiagnostic[] = [];
  try {
    value = lowerDocument(element, options.path, options.onNodeSource, lints);
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
  const validated = validateDocument<PrismatiXUiDocument>(
    actualKind === "scene" ? "ui" : "uiComponent", value, options.path,
  );
  return lints.length === 0 ? validated
    : {...validated, diagnostics: [...lints, ...validated.diagnostics]};
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
