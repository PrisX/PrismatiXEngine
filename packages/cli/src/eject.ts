/**
 * `prismatix eject ui <route>`: turns one of the stock Player's built-in
 * screens (exported by the Runtime as <route>.pxui at 1280x720) into an
 * editable TSX file that keeps every control name and binding the Player
 * relies on, scaled to the project's resolution.
 */

type Json = Record<string, unknown>;

interface UiNodeJson {
  readonly id: string;
  readonly parentId: string | null;
  readonly order: number;
  readonly kind: string;
  readonly runtimeType?: string;
  readonly name: string;
  readonly visible: boolean;
  readonly locked: boolean;
  readonly layout: Record<string, number | string>;
  readonly text?: string;
  readonly appearance?: Record<string, unknown>;
  readonly onClick?: {readonly id: string; readonly arguments: Json};
  readonly accessibilityLabel?: string;
  readonly accessibilityRole?: string;
  readonly accessibilityDescription?: string;
  readonly accessibilityFocusOrder?: number;
  readonly runtimeProperties: Json;
  readonly bindings: Record<string, {readonly path: string; readonly formatter?: string}>;
  readonly itemSource?: Json;
}

export const builtinRoutes = ["title", "hud", "backlog", "save", "load", "gallery", "settings", "video"] as const;
const designWidth = 1280;
const designHeight = 720;

const defaultAppearance: Readonly<Record<string, unknown>> = {
  backgroundColor: "#00000000", textColor: "#FFFFFFFF", opacity: 1, disabledOpacity: 0.5,
};
const plainLayout: Readonly<Record<string, number | string>> = {
  mode: "free", anchorX: 0, anchorY: 0, anchorRight: 0, anchorBottom: 0,
  pivotX: 0, pivotY: 0, margin: 0, alignment: "start", sizeRule: "fixed",
};

function literal(value: unknown): string {
  return JSON.stringify(value);
}

function componentFor(node: UiNodeJson): string {
  if (node.kind === "group" && node.runtimeType === undefined) return "Group";
  if (node.kind === "label" && node.runtimeType === "Label") return "Text";
  if (node.kind === "label" && node.runtimeType === "RichTextLabel") return "RichTextLabel";
  if (node.kind === "button" && (node.runtimeType === undefined || node.runtimeType === "Button")) return "Button";
  if (node.kind === "control" && node.runtimeType === "Slider") return "Slider";
  if (node.kind === "control" && node.runtimeType === "LineEdit") return "TextField";
  return "Control";
}

/**
 * Moves the runtimeProperties every component has a typed prop for out of
 * `runtime` and returns those props, so an ejected screen validates without
 * PXSDKJSX1100 ("has a typed prop") warnings. Offsets of show/hide
 * transitions scale like the Runtime scales the built-in screens.
 */
function typedNodeProps(runtime: Json): string[] {
  const out: string[] = [];
  const take = <T>(key: string, type: "string" | "boolean" | "number"): T | undefined => {
    if (typeof runtime[key] !== type) return undefined;
    const value = runtime[key] as T;
    delete runtime[key];
    return value;
  };
  for (const key of ["focusMode", "mouseFilter", "hoverSoundPath", "activateSoundPath"]) {
    const value = take<string>(key, "string");
    if (value !== undefined) out.push(`${key}=${literal(value)}`);
  }
  for (const key of ["modalCapture", "clipContent", "linearTransition"]) {
    const value = take<boolean>(key, "boolean");
    if (value !== undefined) out.push(value ? key : `${key}={false}`);
  }
  for (const phase of ["show", "hide"] as const) {
    if (typeof runtime[`${phase}TransitionSeconds`] !== "number") continue;
    const seconds = take<number>(`${phase}TransitionSeconds`, "number")!;
    const offsetX = take<number>(`${phase}OffsetX`, "number");
    const offsetY = take<number>(`${phase}OffsetY`, "number");
    const ease = take<string>(`${phase}TransitionEase`, "string");
    const y = offsetY === undefined ? 0 : phase === "show" ? `sf(${offsetY})` : offsetY;
    const fields = [`seconds: ${seconds}`,
      ...(offsetX === undefined && offsetY === undefined ? [] : [`offset: [${offsetX ?? 0}, ${y}]`]),
      ...(ease === undefined ? [] : [`ease: ${literal(ease)}`])];
    out.push(`${phase}Transition={{${fields.join(", ")}}}`);
  }
  return out;
}

/** Returns the TSX source of a built-in screen for a project resolution. */
export function builtinScreenTsx(route: string, document: Json, width: number, height: number): string {
  const nodes = (document.nodes as UiNodeJson[]).slice();
  const children = new Map<string | null, UiNodeJson[]>();
  for (const node of nodes) children.set(node.parentId, [...children.get(node.parentId) ?? [], node]);
  for (const list of children.values()) list.sort((left, right) => left.order - right.order);
  const used = new Set<string>(["Scene"]);

  const attributes = (node: UiNodeJson, root: boolean): string[] => {
    const component = componentFor(node);
    const out = [`name=${literal(node.name)}`];
    if (component === "Control") {
      out.push(`kind=${literal(node.kind)}`);
      if (node.runtimeType !== undefined) out.push(`runtimeType=${literal(node.runtimeType)}`);
    }
    if (!node.visible) out.push("visible={false}");
    if (node.locked) out.push("locked");
    const layout = node.layout;
    const fill = !root && layout.anchorX === 0 && layout.anchorY === 0 && layout.anchorRight === 1 &&
      layout.anchorBottom === 1 && layout.alignment === "fill" && layout.sizeRule === "fill" &&
      layout.x === 0 && layout.y === 0 && layout.mode === "free";
    if (fill) out.push("fill");
    else {
      out.push(`position={[sx(${layout.x}), sy(${layout.y})]}`, `size={[sx(${layout.width}), sy(${layout.height})]}`);
      const rest = Object.fromEntries(Object.entries(layout).filter(([key, value]) =>
        !["x", "y", "width", "height"].includes(key) && (root || value !== plainLayout[key])));
      if (Object.keys(rest).length > 0) out.push(`layout={${literal(rest)}}`);
    }
    if (node.text !== undefined) out.push(`text=${literal(node.text)}`);
    const appearance = Object.fromEntries(Object.entries(node.appearance ?? {})
      .filter(([key, value]) => value !== defaultAppearance[key]));
    if (Object.keys(appearance).length > 0) {
      // Box decoration sizes scale with the screen, like font sizes.
      const scaledKeys = new Set(["cornerRadius", "borderWidth", "shadowBlur", "shadowOffsetX", "shadowOffsetY"]);
      const fields = Object.entries(appearance).map(([key, value]) =>
        `${key}: ${scaledKeys.has(key) && typeof value === "number" ? `sf(${value})` : literal(value)}`);
      out.push(`appearance={{${fields.join(", ")}}}`);
    }
    if (node.onClick !== undefined)
      out.push(Object.keys(node.onClick.arguments).length === 0 ? `action=${literal(node.onClick.id)}`
        : `action={${literal(node.onClick)}}`);
    for (const key of ["accessibilityLabel", "accessibilityRole", "accessibilityDescription"] as const)
      if (node[key] !== undefined) out.push(`${key}=${literal(node[key])}`);
    if (node.accessibilityFocusOrder !== undefined)
      out.push(`accessibilityFocusOrder={${node.accessibilityFocusOrder}}`);
    const runtime: Json = {...node.runtimeProperties};
    out.push(...typedNodeProps(runtime));
    const scaled: string[] = [];
    // Mirrors the Runtime's own scaling of the built-in screens.
    for (const key of ["fontSize", "separation", "letterSpacing", "showOffsetY"])
      if (typeof runtime[key] === "number") {
        const value = runtime[key] as number;
        delete runtime[key];
        if (key === "fontSize" && ["Text", "RichTextLabel", "Button"].includes(component))
          out.push(`fontSize={sf(${value})}`);
        else scaled.push(`${key}: sf(${value})`);
      }
    for (const key of ["itemExtent", "gap"]) {
      const value = runtime[key] as {type?: string; x?: number; y?: number} | undefined;
      if (value?.type === "vec2") {
        delete runtime[key];
        scaled.push(`${key}: {type: "vec2", x: sx(${value.x}), y: sy(${value.y})}`);
      }
    }
    if (component === "RichTextLabel" && typeof runtime.wrap === "boolean") {
      if (runtime.wrap) out.push("wrap"); else out.push("wrap={false}");
      delete runtime.wrap;
    }
    const plain = Object.entries(runtime).map(([key, value]) => `${key}: ${literal(value)}`);
    if (plain.length + scaled.length > 0) out.push(`runtimeProperties={{${[...plain, ...scaled].join(", ")}}}`);
    const bindings = Object.entries(node.bindings);
    if (bindings.length > 0)
      out.push(`bind={{${bindings.map(([property, binding]) => `${literal(property)}: ${
        binding.formatter === undefined ? literal(binding.path)
          : binding.formatter === "Not" ? literal(`!${binding.path}`) : literal(binding)}`).join(", ")}}}`);
    if (node.itemSource !== undefined) out.push(`itemSource={${literal(node.itemSource)}}`);
    return out;
  };

  const render = (node: UiNodeJson, depth: number, root: boolean): string => {
    const component = componentFor(node);
    used.add(component);
    const indent = "  ".repeat(depth);
    const lines = attributes(node, root);
    const open = `${indent}<${component} ${lines.join(`\n${indent}  `)}`;
    const kids = children.get(node.id) ?? [];
    if (kids.length === 0) return `${open} />`;
    return `${open}>\n${kids.map((child) => render(child, depth + 1, false)).join("\n")}\n${indent}</${component}>`;
  };

  const roots = children.get(null) ?? [];
  if (roots.length !== 1) throw new Error(`Built-in ${route} screen must have one root`);
  const body = render(roots[0]!, 2, true);
  const imports = [...used].sort().join(", ");
  return `import {${imports}} from "@prismatix/authoring-sdk";

// Ejected from the stock Player's built-in "${route}" screen (designed at
// ${designWidth}x${designHeight}, scaled to this project). The Player finds controls by
// name (e.g. Dialogue, Slots) and fills the bound UI data, so keep those names
// and bindings when you restyle it; \`prismatix validate\` checks them.
const width = ${width};
const height = ${height};
const sx = (value: number) => value * width / ${designWidth};
const sy = (value: number) => value * height / ${designHeight};
const sf = (value: number) => Math.sign(value) *
  Math.max(1, Math.round(Math.abs(value) * Math.min(width / ${designWidth}, height / ${designHeight})));

export default (
  <Scene name=${literal(String(document.name))} width={width} height={height}>
${body}
  </Scene>
);
`;
}

const routeFiles: Readonly<Record<string, string>> = {
  title: "Title", hud: "HUD", backlog: "Backlog", save: "Save", load: "Load",
  gallery: "Gallery", settings: "Settings", video: "Video",
};

export interface EjectResult {
  readonly created: readonly string[];
  readonly registered: readonly string[];
}

/** Writes Content/UI/<Route>.tsx for each route and registers it in prismatix.json. */
export async function ejectBuiltinUi(root: string, routes: readonly string[], builtinDirectory: string,
  force: boolean): Promise<EjectResult> {
  const {readFile, writeFile, mkdir, access} = await import("node:fs/promises");
  const {join} = await import("node:path");
  const manifestPath = join(root, "prismatix.json");
  const manifest = JSON.parse(await readFile(manifestPath, "utf8")) as Json;
  const resolution = (manifest.resolution ?? {}) as {width?: number; height?: number};
  const width = resolution.width ?? designWidth;
  const height = resolution.height ?? designHeight;
  const entryPoints = {...(manifest.uiEntryPoints ?? {}) as Record<string, string>};
  const created: string[] = [];
  const registered: string[] = [];
  for (const route of routes) {
    if (!(builtinRoutes as readonly string[]).includes(route))
      throw new Error(`No built-in screen named ${route}; choose ${builtinRoutes.join(", ")} or all`);
    const target = `Content/UI/${routeFiles[route]}.tsx`;
    const exists = await access(join(root, target)).then(() => true, () => false);
    if ((exists || entryPoints[route] !== undefined) && !force)
      throw new Error(`${route} is already authored (${entryPoints[route] ?? target}); pass --force to replace it`);
    const document = JSON.parse(await readFile(join(builtinDirectory, `${route}.pxui`), "utf8")) as Json;
    await mkdir(join(root, "Content", "UI"), {recursive: true});
    await writeFile(join(root, target), builtinScreenTsx(route, document, width, height), "utf8");
    created.push(target);
    if (entryPoints[route] !== target) { entryPoints[route] = target; registered.push(route); }
  }
  if (registered.length > 0) {
    manifest.uiEntryPoints = entryPoints;
    await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
  }
  return {created, registered};
}
