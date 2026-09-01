/**
 * The tiny automatic JSX runtime used by PrismatiX UI authoring sources.
 * It deliberately has no React dependency and only builds an authoring tree.
 */

export const uiElementMarker = Symbol.for("prismatix.ui.authoring.element");

export interface UiElement {
  readonly [uiElementMarker]: true;
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
  // Keep a key on the rendered authoring element even when a local function
  // component does not explicitly forward it. The lowering pass can then use
  // the call-site key as a stable identity seed, matching author expectations.
  return key === undefined || rendered.key !== undefined
    ? rendered : {...rendered, key};
}

export const jsxs = jsx;

export function jsxDEV(
  type: unknown,
  props: Record<string, unknown> | null,
  key?: string | number,
): UiElement {
  return jsx(type, props, key);
}

export namespace JSX {
  export type Element = UiElement;
  export type ElementType = typeof Fragment | UiFunctionComponent<never>;
  export interface ElementChildrenAttribute {children: unknown}
  export interface IntrinsicAttributes {readonly key?: string | number}
  export interface IntrinsicElements {}
}
