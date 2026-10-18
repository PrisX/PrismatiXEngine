/**
 * The PrismatiX startup screen every new project starts with: the engine's
 * logo (Resources/Branding/PrismatiXEngine_Logo.png) centred on pure black,
 * fading in and out. `create` writes it as
 * a PrismatiX Studio layout (Content/UI/Splash.studio.json) with its
 * generated TSX, so the Studio's Screens workspace edits it like any screen;
 * PrismatiX Studio's preset library adds the same layout
 * (src/lib/library/splash.ts there).
 */

export const splashRoute = "splash";
export const splashSource = "Content/UI/Splash.tsx";
export const splashStudio = "Content/UI/Splash.studio.json";
export const splashLogo = {
  path: "Content/UI/PrismatiX/PrismatiXEngine_Logo.png",
  size: [1024, 320] as const,
};

/** The `player.splashes` entry that shows it. */
export const splashEntry = {scene: splashSource, minimumDuration: 2.8, skippable: true, skipAllowedAfter: 0.4} as const;

// Fixed ids: the enter/exit animations target the Logo group.
const ids = {
  logo: "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b11",
  enter: "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b12",
  exit: "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b13",
  enterTrack: "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b14",
  exitTrack: "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b15",
  enterState: "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b16",
  exitState: "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b17",
  keys: ["5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b18", "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b19",
    "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b1a", "5b1f0c3e-7a4d-4e2a-9c1b-2f6d8e9a0b1b"],
};

type Json = null | boolean | number | string | Json[] | {[key: string]: Json};

function animations(): Json {
  const track = (id: string, keys: [string, number, number, string][]): Json => ({
    id, nodeId: ids.logo, property: "opacity",
    keys: keys.map(([key, time, value, easing]) => ({id: key, time, value, easing, interpolation: "linear"})),
  });
  return {
    clips: [
      {id: ids.enter, name: "enter", duration: 0.6, loop: false,
        tracks: [track(ids.enterTrack, [[ids.keys[0]!, 0, 0, "easeOut"], [ids.keys[1]!, 0.6, 1, "linear"]])]},
      {id: ids.exit, name: "exit", duration: 0.7, loop: false,
        tracks: [track(ids.exitTrack, [[ids.keys[2]!, 0, 1, "easeIn"], [ids.keys[3]!, 0.7, 0, "linear"]])]},
    ],
    stateMachine: {
      entryStateId: ids.enterState, parameters: [], transitions: [],
      states: [
        {id: ids.enterState, name: "enter", clipId: ids.enter, position: {x: 0, y: 0}},
        {id: ids.exitState, name: "exit", clipId: ids.exit, position: {x: 240, y: 0}},
      ],
    },
  };
}

/** The logo at its own size for 1920x1080, scaled to the project's resolution, centred. */
function layout(width: number, height: number) {
  const k = Math.min(width / 1920, height / 1080);
  const w = Math.round(splashLogo.size[0] * k);
  const h = Math.round(splashLogo.size[1] * k);
  return {logo: {position: [Math.round((width - w) / 2), Math.round((height - h) / 2)], size: [w, h]}};
}

/** The Studio layout document (Content/UI/Splash.studio.json). */
export function splashStudioDocument(width: number, height: number): Json {
  const at = layout(width, height);
  return {
    format: "PrismatiXStudioLayout",
    schemaRevision: 1,
    scene: {kind: "scene", name: "Splash", width, height, extra: {animations: animations()}},
    nodes: [{id: "splash-root", type: "Group", props: {name: "SplashRoot", fill: true}, children: [
      {id: "splash-backdrop", type: "ColorRect", props: {name: "Backdrop", fill: true, color: "#000000FF"}, children: []},
      {id: "splash-logo", type: "Group", props: {name: "Logo", id: ids.logo, fill: true}, children: [
        {id: "splash-image", type: "Image", props: {name: "LogoImage", path: splashLogo.path, ...at.logo, scaleMode: "Fit"}, children: []},
      ]},
    ]}],
  };
}

// A JS literal as the Studio prints one: short values on one line, long
// arrays and objects broken one entry per line.
const key = (name: string) => /^[A-Za-z_$][A-Za-z0-9_$]*$/u.test(name) ? name : JSON.stringify(name);
const print = (value: Json, indent: string, width = 96): string => {
  if (value === null || typeof value !== "object") return JSON.stringify(value);
  const inline = Array.isArray(value)
    ? `[${value.map((item) => print(item, indent, Infinity)).join(", ")}]`
    : `{${Object.entries(value).map(([name, item]) => `${key(name)}: ${print(item, indent, Infinity)}`).join(", ")}}`;
  if (inline.length + indent.length <= width) return inline;
  const inner = `${indent}  `;
  if (Array.isArray(value)) return `[\n${value.map((item) => `${inner}${print(item, inner, width)},`).join("\n")}\n${indent}]`;
  return `{\n${Object.entries(value).map(([name, item]) => `${inner}${key(name)}: ${print(item, inner, width)},`).join("\n")}\n${indent}}`;
};

/** Its TSX (Content/UI/Splash.tsx), as the Studio generates it from the layout. */
export function splashTsx(width: number, height: number): string {
  const at = layout(width, height);
  const pair = (value: readonly number[]) => `[${value.join(", ")}]`;
  return `// Generated by PrismatiX Studio from Splash.studio.json. Edit in the Studio, or delete this line to take over by hand.
import {ColorRect, Group, Image, Scene} from "@prismatix/authoring-sdk";

export default (
  <Scene name="Splash"
    width={${width}}
    height={${height}}
    animations={${print(animations(), "    ")}}>
    <Group name="SplashRoot" fill>
      <ColorRect name="Backdrop" fill color="#000000FF" />
      <Group name="Logo" id="${ids.logo}" fill>
        <Image name="LogoImage"
          position={${pair(at.logo.position)}} size={${pair(at.logo.size)}}
          path="${splashLogo.path}"
          scaleMode="Fit" />
      </Group>
    </Group>
  </Scene>
);
`;
}
