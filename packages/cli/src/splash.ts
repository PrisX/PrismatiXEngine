/**
 * The "Made with PrismatiX" startup screen every new project starts with:
 * the faceted prism over the wordmark on near-black, fading in (the mark
 * grows a little, the wordmark rises after it) and out. `create` writes it as
 * a PrismatiX Studio layout (Content/UI/Splash.studio.json) with its
 * generated TSX, so the Studio's Screens workspace edits it like any screen;
 * PrismatiX Studio's preset library adds the same layout
 * (src/lib/library/splash.ts there).
 */

export const splashRoute = "splash";
export const splashSource = "Content/UI/Splash.tsx";
export const splashStudio = "Content/UI/Splash.studio.json";
export const splashLogo = {
  mark: "Content/UI/PrismatiX/prismatix-mark.png",
  wordmark: "Content/UI/PrismatiX/prismatix-wordmark.png",
  markSize: [960, 880] as const,
  wordmarkSize: [1096, 203] as const,
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

/** Lengths of a 1920x1080 design at the project's resolution. */
function layout(width: number, height: number) {
  const k = Math.min(width / 1920, height / 1080);
  const px = (value: number) => Math.round(value * k);
  const cx = width / 2;
  const cy = height / 2;
  const y = (value: number) => Math.round(cy + px(value));
  const markW = px(132);
  const markH = Math.round(markW * splashLogo.markSize[1] / splashLogo.markSize[0]);
  const wordH = px(46);
  const wordW = Math.round(wordH * splashLogo.wordmarkSize[0] / splashLogo.wordmarkSize[1]);
  return {
    caption: {position: [0, y(-128)], size: [width, px(32)], fontSize: Math.max(10, px(22))},
    mark: {position: [Math.round(cx - markW / 2), y(-74)], size: [markW, markH]},
    wordmark: {position: [Math.round(cx - wordW / 2), y(74)], size: [wordW, wordH], rise: px(12)},
  };
}

/** The Studio layout document (Content/UI/Splash.studio.json). */
export function splashStudioDocument(width: number, height: number): Json {
  const at = layout(width, height);
  return {
    format: "PrismatiXStudioLayout",
    schemaRevision: 1,
    scene: {kind: "scene", name: "Splash", width, height, extra: {animations: animations()}},
    nodes: [{id: "splash-root", type: "Group", props: {name: "SplashRoot", fill: true}, children: [
      {id: "splash-backdrop", type: "ColorRect", props: {name: "Backdrop", fill: true, color: "#08080AFF"}, children: []},
      {id: "splash-logo", type: "Group", props: {name: "Logo", id: ids.logo, fill: true}, children: [
        {id: "splash-caption", type: "Text", props: {name: "Caption", ...at.caption, text: "Made with",
          style: {textColor: "#FFFFFF61"}, horizontalAlignment: "Center", verticalAlignment: "Center",
          showTransition: {seconds: 0.8, offset: [0, 0], ease: "cubic", delay: 0.5}}, children: []},
        {id: "splash-mark", type: "Image", props: {name: "Mark", path: splashLogo.mark, ...at.mark, scaleMode: "Fit",
          showTransition: {seconds: 1.1, offset: [0, 0], ease: "cubic", scale: 0.9}}, children: []},
        {id: "splash-wordmark", type: "Image", props: {name: "Wordmark", path: splashLogo.wordmark,
          position: at.wordmark.position, size: at.wordmark.size, scaleMode: "Fit",
          showTransition: {seconds: 0.9, offset: [0, at.wordmark.rise], ease: "cubic", delay: 0.3}}, children: []},
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
import {ColorRect, Group, Image, Scene, Text} from "@prismatix/authoring-sdk";

export default (
  <Scene name="Splash"
    width={${width}}
    height={${height}}
    animations={${print(animations(), "    ")}}>
    <Group name="SplashRoot" fill>
      <ColorRect name="Backdrop" fill color="#08080AFF" />
      <Group name="Logo" id="${ids.logo}" fill>
        <Text name="Caption"
          position={${pair(at.caption.position)}} size={${pair(at.caption.size)}}
          text="Made with"
          fontSize={${at.caption.fontSize}}
          horizontalAlignment="Center" verticalAlignment="Center"
          style={{color: "#FFFFFF61"}}
          showTransition={{seconds: 0.8, offset: [0, 0], ease: "cubic", delay: 0.5}} />
        <Image name="Mark"
          position={${pair(at.mark.position)}} size={${pair(at.mark.size)}}
          path="${splashLogo.mark}"
          scaleMode="Fit"
          showTransition={{seconds: 1.1, offset: [0, 0], ease: "cubic", scale: 0.9}} />
        <Image name="Wordmark"
          position={${pair(at.wordmark.position)}} size={${pair(at.wordmark.size)}}
          path="${splashLogo.wordmark}"
          scaleMode="Fit"
          showTransition={{seconds: 0.9, offset: [0, ${at.wordmark.rise}], ease: "cubic", delay: 0.3}} />
      </Group>
    </Group>
  </Scene>
);
`;
}
