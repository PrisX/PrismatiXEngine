/**
 * The tiny automatic JSX runtime used by PrismatiX UI authoring sources.
 * It deliberately has no React dependency and only builds an authoring tree.
 */

export const uiElementMarker = Symbol.for("prismatix.ui.authoring.element");
export const uiJsxSourceMarker = Symbol.for("prismatix.ui.authoring.jsx-source");
/** Authoring advice about the props an element was written with. */
export const uiLintMarker = Symbol.for("prismatix.ui.authoring.lint");

export interface UiLint {
  readonly code: string;
  readonly message: string;
  /** Property path inside the lowered node, e.g. `runtimeProperties.clipContent`. */
  readonly property: string;
}

export interface UiJsxSource {
  readonly fileName: string;
  readonly lineNumber: number;
  readonly columnNumber: number;
}

export interface UiElement {
  readonly [uiElementMarker]: true;
  readonly [uiJsxSourceMarker]?: UiJsxSource;
  readonly [uiLintMarker]?: readonly UiLint[];
  readonly type: unknown;
  readonly props: Readonly<Record<string, unknown>>;
  readonly key?: string | number;
}

export type UiChild = UiElement | string | number | null | undefined | boolean |
  readonly UiChild[];

export type UiFunctionComponent<Props = Record<string, unknown>> =
  (props: Props) => UiElement;

export const Fragment = Symbol.for("prismatix.ui.authoring.fragment");

export function isUiElement(value: unknown): value is UiElement {
  return value !== null && typeof value === "object" &&
    (value as Partial<UiElement>)[uiElementMarker] === true;
}

export function jsx(
  type: unknown,
  props: Record<string, unknown> | null,
  key?: string | number,
): UiElement {
  const element: UiElement = {
    [uiElementMarker]: true,
    type,
    props: props ?? {},
    ...(key === undefined ? {} : {key}),
  };
  if (typeof type !== "function") return element;
  const componentProps = key === undefined
    ? element.props
    : {...element.props, key};
  const rendered = (type as UiFunctionComponent<typeof componentProps>)(componentProps);
  if (!isUiElement(rendered)) {
    throw new TypeError("PrismatiX JSX components must return one UI element");
  }
  const lints = runtimePropertyLints(element.props, (type as {name?: string}).name ?? "");
  const linted = lints.length === 0 ? rendered
    : {...rendered, [uiLintMarker]: [...rendered[uiLintMarker] ?? [], ...lints]};
  // Keep a key on the rendered authoring element even when a local function
  // component does not explicitly forward it. The lowering pass can then use
  // the call-site key as a stable identity seed, matching author expectations.
  return key === undefined || linted.key !== undefined
    ? linted : {...linted, key};
}

// runtimeProperties keys that a typed prop now expresses; the typed form is
// checked by TypeScript and survives Runtime renames.
const typedRuntimeProperties: Readonly<Record<string, string>> = {
  activateSoundPath: "activateSoundPath=",
  hoverSoundPath: "hoverSoundPath=",
  focusMode: "focusMode=",
  mouseFilter: "mouseFilter=",
  modalCapture: "modalCapture=",
  clipContent: "clipContent",
  showTransitionSeconds: "showTransition={{seconds}}",
  showOffsetX: "showTransition={{offset}}",
  showOffsetY: "showTransition={{offset}}",
  hideTransitionSeconds: "hideTransition={{seconds}}",
  hideOffsetX: "hideTransition={{offset}}",
  hideOffsetY: "hideTransition={{offset}}",
  linearTransition: "linearTransition",
  showDelaySeconds: "showTransition={{delay}}",
  showWipe: "showTransition={{wipe}}",
  hideWipe: "hideTransition={{wipe}}",
  showScale: "showTransition={{scale}}",
  hideScale: "hideTransition={{scale}}",
  showFade: "showTransition={{fade}}",
  hideFade: "hideTransition={{fade}}",
  staggerSeconds: "stagger=",
  hoverScale: "hoverScale=",
  itemShowTransitionSeconds: "itemShowTransition={{seconds}}",
  itemHoverScale: "itemHoverScale=",
  showSoundPath: "showSound=",
  showSoundVariants: "showSound={{variants}}",
  showSoundVolume: "showSound={{volume}}",
  showSoundDelaySeconds: "showSound={{delay}}",
  hideSoundPath: "hideSound=",
  hideSoundVariants: "hideSound={{variants}}",
  hideSoundVolume: "hideSound={{volume}}",
  normalPath: "normalPath= or skin=",
  hoverPath: "hoverPath= or skin=",
  pressedPath: "pressedPath= or skin=",
  disabledPath: "disabledPath= or skin=",
  scaleMode: "scaleMode=",
  lockAspectRatio: "lockAspectRatio",
  optionKey: "option= on <Slider>",
  fontSize: "fontSize=",
  wrap: "wrap",
  font: "font=",
  letterSpacing: "letterSpacing=",
  outlineSize: "outlineSize=",
  outlineColor: "outlineColor=",
};

const statePaths = ["normalPath", "hoverPath", "pressedPath", "disabledPath"] as const;

// Text styling props exist only on these components.
const textComponents = new Set(["Text", "RichTextLabel", "Button", "TextureButton"]);
const textProperties = new Set(["fontSize", "wrap", "font", "letterSpacing", "outlineSize", "outlineColor"]);

function runtimePropertyLints(props: Readonly<Record<string, unknown>>, component: string): UiLint[] {
  const lints: UiLint[] = [];
  if (props.runtimeType === "Slider")
    lints.push({code: "PXSDKJSX1101", property: "runtimeType",
      message: "Use <Slider> instead of <Control runtimeType=\"Slider\">; option= binds a player.options entry"});
  // Explicit state images that follow the `<stem>-<state>.png` convention.
  const normal = typeof props.normalPath === "string"
    ? /^(.*)-normal(\.[A-Za-z0-9]+)$/u.exec(props.normalPath) : null;
  if (normal !== null && props.skin === undefined && statePaths.slice(1).some((key) => {
    const value = props[key];
    return typeof value === "string" && value.startsWith(`${normal[1]}-`);
  }))
    lints.push({code: "PXSDKJSX1102", property: "runtimeProperties.normalPath",
      message: `State images follow ${normal[1]}-<state>${normal[2]}; write skin="${normal[1]}" and keep only the paths that differ`});
  const authored = props.runtimeProperties;
  if (authored === null || typeof authored !== "object" || Array.isArray(authored)) return lints;
  for (const key of Object.keys(authored)) {
    if (textProperties.has(key) && !textComponents.has(component)) continue;
    const typed = typedRuntimeProperties[key];
    if (typed !== undefined)
      lints.push({code: "PXSDKJSX1100", property: `runtimeProperties.${key}`,
        message: `runtimeProperties.${key} has a typed prop; write ${typed} instead`});
  }
  return lints;
}

export const jsxs = jsx;

export function jsxDEV(
  type: unknown,
  props: Record<string, unknown> | null,
  key?: string | number,
  _isStaticChildren?: boolean,
  source?: UiJsxSource,
): UiElement {
  const rendered = jsx(type, props, key);
  // A component's own JSX location is more useful than its call site. Keep
  // that location when the component returns an authored UI node.
  return source === undefined || rendered[uiJsxSourceMarker] !== undefined
    ? rendered : {...rendered, [uiJsxSourceMarker]: source};
}

export namespace JSX {
  export type Element = UiElement;
  export type ElementType = typeof Fragment | UiFunctionComponent<never>;
  export interface ElementChildrenAttribute {children: unknown}
  export interface IntrinsicAttributes {readonly key?: string | number}
  export interface IntrinsicElements {}
}
