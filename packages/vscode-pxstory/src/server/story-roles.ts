import type {ExtensionCommand, JsonValue, StoryAssetCategory, StoryArgument} from "@prismatix/authoring-sdk";

/** What an argument slot of a Story command refers to. The slots follow the
 * SDK compiler's own `named(...) ?? positional(n)` reads for each command. */
export type ArgumentRole =
  | {readonly kind: "asset"; readonly category?: StoryAssetCategory; readonly extensions?: RegExp}
  | {readonly kind: "character"; readonly extra?: readonly string[]}
  | {readonly kind: "expression"; readonly character?: string; readonly extra?: readonly string[]}
  | {readonly kind: "expressionOrPosition"; readonly character?: string}
  | {readonly kind: "position"}
  | {readonly kind: "label"; readonly scene?: string}
  | {readonly kind: "scene"}
  | {readonly kind: "variable"}
  | {readonly kind: "imageLayer"; readonly extra?: readonly string[]}
  | {readonly kind: "preset"}
  | {readonly kind: "route"}
  | {readonly kind: "values"; readonly values: readonly JsonValue[]}
  | {readonly kind: "none"};

export const positions = ["left", "center", "right"] as const;

export const actorPresets = [
  "hop", "jump", "startle", "yawn", "bow", "nod", "nods", "tremble",
  "shake-head", "sway", "float", "wiggle", "dip", "lift", "nudge-right",
  "nudge-left", "bob", "waver", "stop",
] as const;

const flowCommands = new Set(["jump", "call", "choice"]);

function stringOf(argument: StoryArgument | undefined): string | undefined {
  const value = argument?.value;
  return value === undefined || value === null || typeof value === "object" ? undefined : String(value);
}

/** The slot an argument occupies: its name, or `$n` for the n-th positional. */
export function argumentSlots(args: readonly StoryArgument[]): string[] {
  let positional = 0;
  return args.map((argument) => argument.name ?? `$${positional++}`);
}

export function namedArgument(args: readonly StoryArgument[], name: string): string | undefined {
  return stringOf(args.find((argument) => argument.name === name));
}

export function positionalArgument(args: readonly StoryArgument[], index: number): string | undefined {
  return stringOf(args.filter((argument) => argument.name === undefined)[index]);
}

/**
 * Role of the slot `slot` (an argument name or `$n`) in command `command`,
 * given the other arguments already on the line. `extension` describes an
 * extension command when the name is not built in.
 */
export function commandArgumentRole(command: string, slot: string,
  args: readonly StoryArgument[], extension?: ExtensionCommand): ArgumentRole {
  const character = namedArgument(args, "character") ?? positionalArgument(args, 0);
  switch (command) {
    case "bg":
      if (slot === "asset" || slot === "$0") return {kind: "asset", category: "bg"};
      if (slot === "rule") return {kind: "asset", category: "rule"};
      if (slot === "transition") return {kind: "values", values: [true, false]};
      if (slot === "ease") return {kind: "values", values: easeValues};
      break;
    case "image":
      if (slot === "name" || slot === "$0") return {kind: "imageLayer"};
      if (slot === "asset" || slot === "$1") return {kind: "asset", category: "bg"};
      if (slot === "ease") return {kind: "values", values: easeValues};
      break;
    case "image.hide":
      if (slot === "name" || slot === "$0") return {kind: "imageLayer", extra: ["all"]};
      break;
    case "transition":
      if (slot === "rule") return {kind: "asset", category: "rule"};
      if (slot === "wait") return {kind: "values", values: [true, false]};
      break;
    case "show":
      if (slot === "character" || (slot === "$0" && namedArgument(args, "character") === undefined))
        return {kind: "character"};
      if (slot === "expression") return {kind: "expression", ...(character === undefined ? {} : {character})};
      if (slot.startsWith("$")) return {kind: "expressionOrPosition", ...(character === undefined ? {} : {character})};
      if (slot === "position") return {kind: "position"};
      if (slot === "asset" || slot === "sprite") return {kind: "asset", category: "sprite"};
      if (slot === "transition") return {kind: "values", values: [true, false, "auto"]};
      break;
    case "move":
      if (slot === "character" || slot === "$0") return {kind: "character"};
      if (slot === "position") return {kind: "position"};
      if (slot === "wait") return {kind: "values", values: [true, false]};
      if (slot === "ease") return {kind: "values", values: easeValues};
      break;
    case "hide":
      if (slot === "character" || slot === "$0") return {kind: "character", extra: ["all"]};
      if (slot === "transition") return {kind: "values", values: [true, false]};
      break;
    case "act": {
      const image = namedArgument(args, "image");
      const named = namedArgument(args, "character");
      if (slot === "image") return {kind: "imageLayer"};
      if (slot === "character" || (slot === "$0" && image === undefined && named === undefined))
        return {kind: "character", extra: ["window", "bg"]};
      if (slot === "preset" || slot === (image === undefined && named === undefined ? "$1" : "$0"))
        return {kind: "preset"};
      if (slot === "ease") return {kind: "values", values: ["linear", "in", "out", "inout"]};
      break;
    }
    case "bgm": case "ambience": case "voice": case "se": {
      const category: StoryAssetCategory = command === "se" ? "se" : command;
      if (slot === "asset" || slot === "$0" || slot === "intro") return {kind: "asset", category};
      if (slot === "loop") return {kind: "values", values: [true, false]};
      break;
    }
    case "video":
      if (slot === "asset" || slot === "$0") return {kind: "asset", category: "video"};
      if (slot === "skippable") return {kind: "values", values: [true, false]};
      break;
    case "timeline":
      if (slot === "timeline" || slot === "$0") return {kind: "asset", extensions: /\.pxtimeline$/iu};
      if (slot === "await") return {kind: "values", values: [true, false]};
      break;
    case "motion":
      if (slot === "asset" || slot === "$0") return {kind: "asset", extensions: /\.pxmotion$/iu};
      if (slot === "await") return {kind: "values", values: [true, false]};
      break;
    case "jump": case "call": case "choice":
      if (slot === "scene") return {kind: "scene"};
      if (slot === "target" || slot === "goto" || (command !== "choice" && slot === "$0")) {
        const scene = namedArgument(args, "scene");
        return {kind: "label", ...(scene === undefined ? {} : {scene})};
      }
      break;
    case "say":
      if (slot === "character" || slot === "speaker") return {kind: "character"};
      break;
    case "set":
      if (slot === "name" || (slot !== "value" && !slot.startsWith("$"))) return {kind: "variable"};
      break;
    case "add":
      if (slot === "name" || slot === "$0") return {kind: "variable"};
      break;
    case "if":
      return {kind: "variable"};
    case "ui":
      if (slot === "route" || slot === "$0") return {kind: "route"};
      // Operations the native RuntimeSession route command understands.
      if (slot === "operation") return {kind: "values", values: ["push", "replace", "modal", "back"]};
      break;
    case "hud":
      if (slot === "visible" || slot === "$0") return {kind: "values", values: [true, false]};
      break;
    case "auto":
      if (slot === "mode" || slot === "$0") return {kind: "values", values: ["on", "off"]};
      if (slot === "lock") return {kind: "values", values: [true, false]};
      break;
    case "shake":
      if (slot === "target") return {kind: "values", values: ["stage", "window"]};
      if (slot === "dir") return {kind: "values", values: ["h", "v", "r"]};
      break;
    case "tone":
      if (slot === "mode" || slot === "$0") return {kind: "values", values: ["gray", "sepia", "none"]};
      break;
    case "camera":
      if (slot === "wait") return {kind: "values", values: [true, false]};
      break;
    default:
      if (extension !== undefined) {
        const parameter = extension.parameters.find((candidate) => candidate.name === slot);
        if (parameter?.enum !== undefined) return {kind: "values", values: parameter.enum};
        if (parameter?.type === "resource") return {kind: "asset"};
        if (parameter?.type === "boolean") return {kind: "values", values: [true, false]};
      }
  }
  return {kind: "none"};
}

/** Role of an inline `@alias[...]` speaker option. */
export function speakerOptionRole(slot: string, character: string | undefined): ArgumentRole {
  const owner = character === undefined ? {} : {character};
  if (slot === "voice") return {kind: "asset", category: "voice"};
  if (slot === "act") return {kind: "preset"};
  if (slot === "face") return {kind: "expression", ...owner, extra: ["none"]};
  if (slot === "expression") return {kind: "expression", ...owner};
  if (slot === "position") return {kind: "position"};
  if (slot.startsWith("$")) return {kind: "expressionOrPosition", ...owner};
  return {kind: "none"};
}

export const speakerOptionNames = ["expression", "position", "voice", "act", "face"] as const;

/** Easing names the native Stage understands (Engine/Support/Easing.h). */
export const easeValues = ["linear", "inQuad", "outQuad", "inOutQuad", "inCubic", "outCubic",
  "inOutCubic", "inQuart", "outQuart", "inOutQuart", "inSine", "outSine", "inOutSine", "inExpo",
  "outExpo", "inOutExpo", "inBack", "outBack", "inOutBack", "inElastic", "outElastic", "inBounce",
  "outBounce", "smoothstep"] as const;

export function isFlowCommand(command: string): boolean { return flowCommands.has(command); }

/** Inline text tags recognized inside dialogue and narration. */
export const inlineTags: readonly {readonly label: string; readonly insert: string; readonly detail: string}[] = [
  {label: "b", insert: "b]", detail: "bold until [/b]"},
  {label: "i", insert: "i]", detail: "italic until [/i]"},
  {label: "br", insert: "br]", detail: "line break"},
  {label: "size=", insert: "size=", detail: "text size: 150% of the enclosing size, or 40 logical pixels"},
  {label: "color=", insert: "color=#", detail: "text colour #RRGGBB or #RRGGBBAA"},
  {label: "font=", insert: "font=", detail: "font file by project-relative path"},
  {label: "ruby=", insert: "ruby=", detail: "ruby reading over the base text until [/ruby]"},
  {label: "/b", insert: "/b]", detail: "end bold"},
  {label: "/i", insert: "/i]", detail: "end italic"},
  {label: "/size", insert: "/size]", detail: "end size"},
  {label: "/color", insert: "/color]", detail: "end colour"},
  {label: "/font", insert: "/font]", detail: "end font"},
  {label: "/ruby", insert: "/ruby]", detail: "end ruby"},
];

/** One-line summaries of the built-in Story commands (see docs/authoring.md). */
export const commandSummaries: Readonly<Record<string, string>> = {
  id: "Pins the save identity of the next observable operation.",
  chapter: "Starts a chapter and shows its title.",
  say: "Speaks one line as a character or a named speaker.",
  narrate: "Shows one line of narration.",
  bg: "Changes the background image, optionally through a rule transition, framed or panned.",
  image: "Places a named image layer (CG, effect) with framing, alpha and z order.",
  "image.hide": "Fades out and removes a named image layer, or all of them.",
  transition: "Reveals every Stage change since the last frame at once, as a crossfade or through a rule image.",
  show: "Shows a character; words after the alias are its expression or position.",
  move: "Moves a visible character to a position or offset over a duration.",
  hide: "Fades out a character, or all characters.",
  choice: "Adds a choice; `goto` names the label (and `scene` the scene) it jumps to.",
  "choice.wait": "Shows the pending choices and waits for the player.",
  jump: "Jumps to a label, or to another scene with `scene=`.",
  call: "Calls a label (or another scene's label) and returns with [return].",
  return: "Returns from a [call].",
  end: "Ends the Story.",
  wait: "Waits for a duration (ms or s).",
  set: "Assigns a declared game variable: [set affection=1].",
  add: "Adds an integer amount to a declared variable.",
  if: "Runs the following lines when the expression is true, until [else] or [endif].",
  else: "Starts the branch taken when the [if] expression is false.",
  endif: "Ends an [if] block.",
  voice: "Plays a voice line.",
  "voice.stop": "Stops the current voice.",
  bgm: "Plays background music; `intro=` plays once before the loop.",
  "bgm.stop": "Stops the background music, optionally fading.",
  se: "Plays a sound effect; `channel=` gives it a replaceable, loopable channel.",
  "se.stop": "Stops every sound effect, or one channel.",
  ambience: "Plays a background ambience loop.",
  "ambience.stop": "Stops the ambience loop.",
  video: "Plays a full-screen video.",
  timeline: "Plays a registered .pxtimeline clip.",
  motion: "Plays a .pxmotion image animation.",
  "motion.stop": "Stops the running .pxmotion animation.",
  ui: "Pushes, pops or replaces a UI route.",
  hud: "Hides or shows the message window and HUD.",
  unlock: "Unlocks a gallery or other unlockable entry.",
  effect: "Applies a Stage effect preset.",
  flash: "Washes the screen with a colour, rising to `alpha` and back.",
  tone: "Sets a whole-screen tone: gray, sepia or none.",
  shake: "Shakes the Stage or the message window.",
  camera: "Moves or zooms the camera.",
  act: "Plays a relative action preset on a character, the background, an image layer or the window.",
  nvl: "Switches to full-screen (NVL) text.",
  "nvl.clear": "Starts a new NVL page.",
  adv: "Returns to the message window.",
  auto: "Starts or stops auto reading; `lock=true` ignores the player until [auto off].",
  "skip.stop": "Ends skipping.",
  "skip.disable": "Refuses skip until [skip.enable].",
  "skip.enable": "Allows skip again.",
  macro: "Defines a Story macro that expands where it is called; ends at [endmacro].",
  endmacro: "Ends a [macro] definition.",
  elseif: "Starts a branch taken when earlier [if]/[elseif] expressions were false and this one is true.",
  endchoice: "Ends a block choice started with [choice text=...] on its own line.",
  var: "Declares a variable with its default: [var affection=0], scope=profile to keep it across games.",
  face: "Changes only a layered character's expression, keeping its position and outfit.",
  credits: "Rolls a staff roll from the lines up to [endcredits].",
  endcredits: "Ends a [credits] block.",
  input: "Asks the player for text and stores it in a string variable: [input var=hero prompt=\"名前\"].",
};
