import type {JsonValue, ExtensionCommand, ExtensionParameter} from "./types.js";
import type {StoryAssetCategory} from "./story-assets.js";
import {storyActorActions} from "./story.js";

/**
 * A typed description of every built-in Story command, for editors: which
 * family a command belongs to, which arguments it takes, how the short form
 * writes them without names, and which values compileStory accepts. Ranges
 * and positions follow compileStory exactly; when the two disagree, that is
 * a bug in this file.
 *
 * Values are described as authored (strings): a duration is `500ms`, `1.5s`
 * or a bare number of milliseconds; a boolean is `true` or `false`.
 */

export type StoryCommandFamily = "text" | "stage" | "audio" | "flow" | "effect" | "system";

export const storyCommandFamilies: readonly StoryCommandFamily[] =
  ["text", "stage", "audio", "flow", "effect", "system"];

/** Canonical easing names of the native Stage (Engine/Support/Easing.h). */
export const storyEaseNames = [
  "linear", "inQuad", "outQuad", "inOutQuad", "inCubic", "outCubic", "inOutCubic",
  "inQuart", "outQuart", "inOutQuart", "inSine", "outSine", "inOutSine",
  "inExpo", "outExpo", "inOutExpo", "inBack", "outBack", "inOutBack",
  "inElastic", "outElastic", "inBounce", "outBounce", "smoothstep",
] as const;

/** Short Artemis-style eases [act] also accepts (none = linear, in = inQuad, ...). */
export const storyActorEaseAliases = ["none", "in", "out", "inout"] as const;

/** Stage slots for characters; `centre`, `1`, `2` and `3` are accepted too. */
export const storyCharacterPositions = ["left", "center", "right"] as const;

export type StoryValueKind =
  /** A resource that may use a short name through `storyAssets` roots. */
  | {readonly type: "asset"; readonly category: StoryAssetCategory}
  /** A project-relative resource path (or asset:/uuid: identity). */
  | {readonly type: "resource"; readonly extensions: readonly string[]; readonly filter?: string}
  /**
   * A character alias, id or unique display name. `allowAll` accepts `all`;
   * `targets` lists non-character targets ([act] takes `window` and `bg`).
   */
  | {readonly type: "character"; readonly allowAll?: boolean; readonly allowTargets?: boolean;
    readonly targets?: readonly string[]}
  /** An expression id, alias or name of the command's character. */
  | {readonly type: "expression"}
  | {readonly type: "position"; readonly values: readonly string[]}
  /** A label in this Story. */
  | {readonly type: "label"}
  /** A scene id from the story index; `values` lists keywords such as `next`. */
  | {readonly type: "scene"; readonly values?: readonly string[]}
  /** Milliseconds by default; `ms` and `s` suffixes. Bounds are inclusive. */
  | {readonly type: "duration"; readonly minMs: number; readonly maxMs: number}
  | {readonly type: "number"; readonly min?: number; readonly max?: number;
    /** `min` itself is not allowed (the value must be greater). */
    readonly exclusiveMin?: boolean; readonly unit?: string}
  | {readonly type: "integer"; readonly min?: number; readonly max?: number;
    /** Values inside [min, max] that are still rejected. */
    readonly excluded?: readonly number[]; readonly unit?: string}
  | {readonly type: "boolean"}
  | {readonly type: "enum"; readonly values: readonly string[]}
  /** `#RRGGBB` (the `#` is optional), with `alpha` also `#RRGGBBAA`. */
  | {readonly type: "color"; readonly alpha: boolean}
  /** A Story condition expression (`affection >= 3 && met_aoi`). */
  | {readonly type: "condition"}
  /** A game variable, optionally of one type. */
  | {readonly type: "variable"; readonly varType?: "boolean" | "integer" | "number" | "string"}
  | {readonly type: "text"; readonly multiline?: boolean}
  /** Two numbers written `x,y`. */
  | {readonly type: "vec2"}
  | {readonly type: "ease"; readonly values: readonly string[]}
  /** The name of an [image] layer; `allowAll` accepts `all`. */
  | {readonly type: "imageLayer"; readonly allowAll?: boolean}
  /** A Player UI route (`save`, `load`, `config`, a project screen...). */
  | {readonly type: "route"}
  /** A stable identifier: letters, digits, `.`, `_` and `-`, starting with a letter or digit. */
  | {readonly type: "identifier"}
  /** A JSON value (extension parameters of type array, object, rect or null). */
  | {readonly type: "json"};

export interface StoryCommandParameter {
  readonly name: string;
  readonly kind: StoryValueKind;
  /**
   * Where the short form writes this argument without its name. With the
   * command's `positionals: "fixed"` it is the index among unnamed arguments;
   * with `"shift"` unnamed arguments fill the positional parameters in order,
   * skipping the ones written by name or left out because of `conflicts`.
   */
  readonly positional?: number;
  /** Takes every remaining unnamed word (joined with spaces). */
  readonly rest?: boolean;
  readonly required?: boolean;
  /** What the engine uses when the argument is left out, as authored text. */
  readonly default?: string;
  /** Other names compileStory reads for the same argument. */
  readonly aliases?: readonly string[];
  /** Only valid together with these arguments. */
  readonly requires?: readonly string[];
  /** Not valid together with these arguments. */
  readonly conflicts?: readonly string[];
  /** Rarely needed; editors may fold it away. */
  readonly advanced?: boolean;
  readonly description?: string;
}

export interface StoryCommandSchema {
  readonly name: string;
  readonly family: StoryCommandFamily;
  readonly description: string;
  readonly parameters: readonly StoryCommandParameter[];
  /** How unnamed arguments map to parameters (see StoryCommandParameter.positional). */
  readonly positionals: "fixed" | "shift";
  /** Unnamed arguments at these parameters may come in any order (told apart by value). */
  readonly anyOrder?: readonly string[];
  /** At least one of these arguments must be written. */
  readonly requiresAnyOf?: readonly string[];
  /** A multi-line block closed by `[end<block>]`; `choice` only when bare. */
  readonly block?: "choice" | "macro" | "credits";
  /** This command closes a block. */
  readonly closes?: "choice" | "macro" | "credits";
  /** The text after the name is one expression, not name=value arguments. */
  readonly expression?: boolean;
  /** Takes one free `variable=value` argument besides its parameters. */
  readonly assignment?: boolean;
  /** Further unnamed or name=default arguments declare macro parameters. */
  readonly variadic?: boolean;
  /** Not offered in insert menus: legacy, internal or written by another form. */
  readonly hidden?: boolean;
  /** Declared by a .pxextension manifest. */
  readonly extension?: boolean;
}

const maximumDurationMs = 2_147_483_647;
const maximumSafeInteger = Number.MAX_SAFE_INTEGER;

const duration = (maxMs = maximumDurationMs, minMs = 0): StoryValueKind =>
  ({type: "duration", minMs, maxMs});
const number = (extra: Omit<Extract<StoryValueKind, {type: "number"}>, "type"> = {}): StoryValueKind =>
  ({type: "number", ...extra});
const integer = (min?: number, max?: number,
  extra: Omit<Extract<StoryValueKind, {type: "integer"}>, "type" | "min" | "max"> = {}): StoryValueKind =>
  ({type: "integer", ...(min === undefined ? {} : {min}), ...(max === undefined ? {} : {max}), ...extra});
const boolean: StoryValueKind = {type: "boolean"};
const enumeration = (...values: string[]): StoryValueKind => ({type: "enum", values});
const asset = (category: StoryAssetCategory): StoryValueKind => ({type: "asset", category});
const text: StoryValueKind = {type: "text"};
const identifier: StoryValueKind = {type: "identifier"};
const ease: StoryValueKind = {type: "ease", values: storyEaseNames};
const position: StoryValueKind = {type: "position", values: storyCharacterPositions};

const p = (name: string, kind: StoryValueKind,
  extra: Omit<StoryCommandParameter, "name" | "kind"> = {}): StoryCommandParameter => ({name, kind, ...extra});

const command = (name: string, family: StoryCommandFamily, description: string,
  parameters: readonly StoryCommandParameter[] = [],
  extra: Omit<StoryCommandSchema, "name" | "family" | "description" | "parameters" | "positionals"> &
    {readonly positionals?: "fixed" | "shift"} = {}): StoryCommandSchema =>
  ({name, family, description, parameters, positionals: "fixed", ...extra});

/** [bg] and [image] framing: top-left position at zoom 1, zoom and rotation about an anchor. */
function framing(withAlpha: boolean): StoryCommandParameter[] {
  const coordinate = number({unit: "px"});
  const zoom = number({min: 0, exclusiveMin: true});
  const alpha = number({min: 0, max: 1});
  return [
    p("x", coordinate, {advanced: true}),
    p("y", coordinate, {advanced: true}),
    p("zoom", zoom, {advanced: true}),
    p("rotate", number({unit: "deg"}), {advanced: true}),
    p("anchor", {type: "vec2"}, {advanced: true, description: "Zoom and rotation centre in image pixels."}),
    ...(withAlpha ? [p("alpha", alpha, {advanced: true, default: "1"})] : []),
    p("ease", ease, {advanced: true, default: "outCubic"}),
    p("from-x", coordinate, {advanced: true}),
    p("from-y", coordinate, {advanced: true}),
    p("from-zoom", zoom, {advanced: true}),
    ...(withAlpha ? [p("from-alpha", alpha, {advanced: true})] : []),
  ];
}

const volume = p("volume", number({min: 0, max: 100, unit: "%"}), {default: "100",
  description: "Per-play gain in percent of the bus volume."});

const builtinSchema: readonly StoryCommandSchema[] = [
  // Text -----------------------------------------------------------------------
  command("say", "text", "A dialogue line written as a command.", [
    p("speaker", text, {required: true}),
    p("text", {type: "text", multiline: true}, {required: true}),
    p("character", {type: "character"}),
  ], {hidden: true}),
  command("narrate", "text", "A narration line written as a command.", [
    p("text", {type: "text", multiline: true}, {required: true}),
  ], {hidden: true}),
  command("nvl", "text", "Full-screen text: following lines collect on a page."),
  command("nvl.clear", "text", "Starts a new full-screen text page."),
  command("adv", "text", "Returns to the message window."),
  command("credits", "text", "A staff roll: one text line, # heading or [image name] per line.", [
    p("duration", duration(600_000, 1_000), {positional: 0, default: "30s"}),
    p("skippable", boolean, {default: "true"}),
    p("size", integer(8, 200, {unit: "px"}), {default: "32"}),
    p("color", {type: "color", alpha: true}),
    p("font", {type: "resource", extensions: [".ttf", ".otf"]}),
  ], {block: "credits"}),
  command("endcredits", "text", "Closes a [credits] block.", [], {closes: "credits", hidden: true}),

  // Stage ----------------------------------------------------------------------
  command("bg", "stage", "Changes the background.", [
    p("asset", asset("bg"), {positional: 0, required: true}),
    p("rule", asset("rule")),
    p("duration", duration(), {description: "Fade time; 600ms with a rule when left out."}),
    p("vague", integer(0, 255), {default: "64", description: "Rule edge softness."}),
    p("transition", boolean, {default: "true",
      description: "false changes the background without a fade (for a later [transition])."}),
    ...framing(false),
  ]),
  command("image", "stage", "Shows or updates a named image layer above the background.", [
    p("name", identifier, {positional: 0, required: true}),
    p("asset", asset("bg"), {positional: 1, required: true}),
    p("duration", duration()),
    p("z", integer(-9999, 9999)),
    ...framing(true),
  ]),
  command("image.hide", "stage", "Hides an image layer, or all of them.", [
    p("name", {type: "imageLayer", allowAll: true}, {positional: 0, required: true}),
    p("duration", duration(), {positional: 1}),
  ]),
  command("show", "stage", "Shows a character, or changes its expression or position.", [
    p("character", {type: "character"}, {positional: 0, required: true}),
    p("expression", {type: "expression"}, {positional: 1}),
    p("position", position, {positional: 2}),
    p("transition", enumeration("true", "false", "auto"), {default: "true"}),
    p("x", number({unit: "px"}), {advanced: true}),
    p("y", number({unit: "px"}), {advanced: true}),
    p("scale", number({min: 0, exclusiveMin: true}), {advanced: true}),
    p("rotate", number({unit: "deg"}), {advanced: true}),
    p("asset", asset("sprite"), {aliases: ["sprite"], advanced: true,
      description: "An image to show instead of the expression's."}),
  ], {positionals: "shift", anyOrder: ["expression", "position"]}),
  command("hide", "stage", "Hides a character (or all), or an image layer with image=.", [
    p("character", {type: "character", allowAll: true}, {positional: 0, conflicts: ["image"]}),
    p("transition", boolean, {default: "true", conflicts: ["image"]}),
    p("image", {type: "imageLayer", allowAll: true}, {advanced: true, conflicts: ["character"]}),
    p("duration", duration(), {positional: 1, requires: ["image"], advanced: true}),
  ], {positionals: "shift", requiresAnyOf: ["character", "image"]}),
  command("move", "stage", "Moves, scales or turns a character on stage.", [
    p("character", {type: "character"}, {positional: 0, required: true}),
    p("position", position),
    p("x", number({unit: "px"})),
    p("y", number({unit: "px"})),
    p("scale", number({min: 0, exclusiveMin: true})),
    p("rotate", number({unit: "deg"})),
    p("duration", duration(), {default: "600ms"}),
    p("ease", ease, {default: "outCubic"}),
    p("wait", boolean, {default: "false"}),
  ], {requiresAnyOf: ["position", "x", "y", "scale", "rotate"]}),
  command("face", "stage", "Changes only the face of a layered character.", [
    p("character", {type: "character"}, {positional: 0, required: true}),
    p("face", {type: "expression"}, {positional: 1, required: true}),
    p("transition", enumeration("true", "false", "auto")),
  ], {positionals: "shift"}),
  command("act", "stage", "Plays a relative action on a character, the window, the background or an image.", [
    p("character", {type: "character", allowTargets: true, targets: ["window", "bg"]},
      {positional: 0, conflicts: ["image"]}),
    p("preset", enumeration(...storyActorActions), {positional: 1, required: true}),
    p("size", number({min: -4096, max: 4096})),
    p("time", duration(600_000)),
    p("loop", integer(-1, 1000, {excluded: [0]}), {default: "1",
      description: "-1 repeats until [act ... stop]."}),
    p("ease", {type: "ease", values: [...storyActorEaseAliases, ...storyEaseNames]}),
    p("image", {type: "imageLayer"}, {advanced: true, conflicts: ["character"]}),
  ], {positionals: "shift", requiresAnyOf: ["character", "image"]}),
  command("camera", "stage", "Pans and zooms the whole stage, or plays a camera preset.", [
    p("x", number({unit: "px"})),
    p("y", number({unit: "px"})),
    p("zoom", number({min: 0, exclusiveMin: true}), {default: "1"}),
    p("duration", duration(), {default: "600ms"}),
    p("ease", ease, {default: "outCubic"}),
    p("wait", boolean, {default: "true"}),
    p("value", text, {positional: 0, advanced: true, conflicts: ["x", "y", "zoom"],
      description: "A camera preset name."}),
  ], {requiresAnyOf: ["value", "x", "y", "zoom"]}),

  // Audio ----------------------------------------------------------------------
  command("bgm", "audio", "Plays background music.", [
    p("asset", asset("bgm"), {positional: 0, required: true}),
    p("fade", duration()),
    p("loop", boolean, {default: "true"}),
    p("intro", asset("bgm"), {description: "Plays once before the looping body; not with loop=false."}),
    volume,
  ]),
  command("bgm.stop", "audio", "Stops the music.", [p("fade", duration(), {positional: 0})]),
  command("se", "audio", "Plays a sound effect; channel= makes it loop- and fade-capable.", [
    p("asset", asset("se"), {positional: 0, required: true}),
    p("channel", integer(1, 64)),
    p("loop", boolean, {default: "false", requires: ["channel"]}),
    p("fade", duration(), {requires: ["channel"]}),
    volume,
  ]),
  command("se.stop", "audio", "Stops sound effects (one channel with channel=).", [
    p("fade", duration(), {positional: 0}),
    p("channel", integer(1, 64)),
  ]),
  command("voice", "audio", "Plays a voice line.", [
    p("asset", asset("voice"), {positional: 0, required: true}),
    volume,
  ]),
  command("voice.stop", "audio", "Stops the voice."),
  command("ambience", "audio", "Plays a looping ambient sound.", [
    p("asset", asset("ambience"), {positional: 0, required: true}),
    p("fade", duration()),
    p("loop", boolean, {default: "true"}),
    volume,
  ]),
  command("ambience.stop", "audio", "Stops the ambience.", [p("fade", duration(), {positional: 0})]),

  // Flow -----------------------------------------------------------------------
  command("choice", "flow", "A choice option; a bare [choice] opens a `- text -> label` block.", [
    p("text", text, {required: true}),
    p("goto", {type: "label"}, {positional: 0, aliases: ["target"]}),
    p("scene", {type: "scene", values: ["next"]}),
    p("if", {type: "condition"}, {description: "Hides the option while false."}),
  ], {block: "choice", requiresAnyOf: ["goto", "scene"]}),
  command("endchoice", "flow", "Closes a [choice] block.", [], {closes: "choice", hidden: true}),
  command("choice.wait", "flow", "Waits for the choices written so far.", [], {hidden: true}),
  command("jump", "flow", "Continues at a label, or at another scene with scene=.", [
    p("target", {type: "label"}, {positional: 0, aliases: ["goto"]}),
    p("scene", {type: "scene", values: ["next"]}),
  ], {requiresAnyOf: ["target", "scene"]}),
  command("call", "flow", "Runs a label (or scene) and comes back at its [return].", [
    p("target", {type: "label"}, {positional: 0, aliases: ["goto"]}),
    p("scene", {type: "scene", values: ["next"]}),
  ], {requiresAnyOf: ["target", "scene"]}),
  command("return", "flow", "Returns from a [call]."),
  command("end", "flow", "Ends the story."),
  command("if", "flow", "Runs the following lines while the condition holds.", [
    p("condition", {type: "condition"}, {positional: 0, rest: true, required: true}),
  ], {expression: true}),
  command("elseif", "flow", "Another condition in an open [if].", [
    p("condition", {type: "condition"}, {positional: 0, rest: true, required: true}),
  ], {expression: true}),
  command("else", "flow", "The lines run when no condition of the [if] held."),
  command("endif", "flow", "Closes an [if]."),
  command("chapter", "flow", "Starts a chapter; every word is the title.", [
    p("title", text, {positional: 0, rest: true, required: true}),
  ]),
  command("wait", "flow", "Pauses the story.", [
    p("duration", duration(), {positional: 0, required: true}),
  ]),

  // Effect ---------------------------------------------------------------------
  command("transition", "effect", "Reveals every change since the last shown frame together.", [
    p("duration", duration(), {positional: 0, default: "500ms"}),
    p("rule", asset("rule")),
    p("vague", integer(0, 255), {default: "64"}),
    p("wait", boolean, {default: "true"}),
    p("effect", identifier, {conflicts: ["rule", "vague"],
      description: "A project transition effect (.pxeffect); its uniforms are further name=value arguments."}),
  ]),
  command("shake", "effect", "Shakes the stage or the message window.", [
    p("duration", duration(), {positional: 0, required: true}),
    p("amplitude", integer(undefined, undefined, {unit: "px"}), {positional: 1, default: "12"}),
    p("target", enumeration("stage", "window"), {default: "stage"}),
    p("dir", enumeration("h", "v", "r"), {description: "Switches to a quake that swings count times."}),
    p("count", integer(1, 100), {requires: ["dir"]}),
  ]),
  command("flash", "effect", "Flashes the screen with a colour.", [
    p("color", {type: "color", alpha: false}, {default: "ffffff"}),
    p("alpha", number({min: 0, max: 1}), {default: "1"}),
    p("duration", duration(), {positional: 0, default: "200ms"}),
    p("loop", integer(1, 99), {default: "1"}),
  ]),
  command("tone", "effect", "Tints the whole screen.", [
    p("mode", enumeration("gray", "sepia", "none"), {positional: 0, required: true}),
  ]),
  command("effect", "effect", "Plays a screen effect preset or a project Stage effect; none removes the project effect.", [
    p("value", text, {positional: 0, required: true}),
    p("duration", duration(), {positional: 1,
      description: "How long the effect takes to come in. A project effect takes its uniforms as name=value arguments."}),
  ]),
  command("motion", "effect", "Plays a .pxmotion clip on the stage.", [
    p("asset", {type: "resource", extensions: [".pxmotion"]}, {positional: 0, required: true}),
    p("await", boolean, {default: "false"}),
    p("speed", number({min: 0, max: 16, exclusiveMin: true}), {default: "1"}),
  ]),
  command("motion.stop", "effect", "Stops stage motion."),
  command("timeline", "effect", "Plays a timeline or animation clip.", [
    p("timeline", {type: "resource", extensions: [".pxtimeline", ".pxanim"]}, {positional: 0, required: true}),
    p("await", boolean, {default: "false"}),
    p("speed", number({min: 0, exclusiveMin: true}), {default: "1"}),
  ]),
  command("video", "effect", "Plays a full-screen video.", [
    p("asset", asset("video"), {positional: 0, required: true}),
    p("skippable", boolean, {default: "true"}),
  ]),

  // System ---------------------------------------------------------------------
  command("var", "system", "Declares a game variable: [var affection=0 scope=profile].", [
    p("type", enumeration("boolean", "integer", "number", "string"),
      {description: "Inferred from the default when left out."}),
    p("scope", enumeration("session", "profile"), {default: "session"}),
  ], {assignment: true}),
  command("set", "system", "Sets a variable: [set affection=3].", [
    p("name", {type: "variable"}),
    p("value", text),
  ], {assignment: true}),
  command("add", "system", "Adds to an integer variable.", [
    p("name", {type: "variable", varType: "integer"}, {positional: 0, required: true}),
    p("amount", integer(-maximumSafeInteger, maximumSafeInteger), {positional: 1, required: true}),
  ]),
  command("input", "system", "Asks the player for text and stores it in a string variable.", [
    p("var", {type: "variable", varType: "string"}, {positional: 0, required: true}),
    p("prompt", text),
    p("default", text),
    p("max", integer(1, 256), {default: "32"}),
  ]),
  command("hud", "system", "Shows or hides the message window and HUD.", [
    p("visible", boolean, {positional: 0, required: true}),
    p("time", duration(10_000), {positional: 1, default: "200ms"}),
  ]),
  command("ui", "system", "Opens a Player UI route.", [
    p("route", {type: "route"}, {positional: 0, required: true}),
    p("operation", enumeration("push", "replace", "modal", "back"), {default: "push"}),
  ]),
  command("auto", "system", "Turns story-driven auto play on or off.", [
    p("mode", enumeration("on", "off"), {positional: 0, required: true}),
    p("speed", integer(0, 1000, {unit: "ms/char"})),
    p("wait", duration(60_000)),
    p("lock", boolean, {description: "Also ignores the player until [auto off]."}),
  ]),
  command("skip.stop", "system", "Stops skipping."),
  command("skip.disable", "system", "Disables skipping."),
  command("skip.enable", "system", "Enables skipping again."),
  command("unlock", "system", "Unlocks a gallery item, scene or other extra.", [
    p("kind", identifier, {positional: 0, required: true}),
    p("id", identifier, {positional: 1, required: true}),
  ]),
  command("macro", "system", "Defines a macro: [macro name param other=default] ... [endmacro].", [
    p("name", identifier, {positional: 0, required: true}),
  ], {block: "macro", variadic: true}),
  command("endmacro", "system", "Closes a [macro].", [], {closes: "macro", hidden: true}),
  command("id", "system", "Pins the save identity of the next operation.", [
    p("value", identifier, {positional: 0, required: true}),
  ], {hidden: true}),
];

function extensionKind(parameter: ExtensionParameter): StoryValueKind {
  if (parameter.enum !== undefined)
    return {type: "enum", values: parameter.enum.map((value) => typeof value === "string" ? value : JSON.stringify(value))};
  const range = parameter.range;
  switch (parameter.type) {
    case "boolean": return boolean;
    case "integer":
    case "number":
      return {type: parameter.type,
        ...(range?.minimum === undefined ? {} : {min: range.minimum}),
        ...(range?.maximum === undefined ? {} : {max: range.maximum})};
    case "string": return text;
    case "token":
    case "uuid": return identifier;
    case "resource": return {type: "resource", extensions: [],
      ...(parameter.resourceFilter === undefined ? {} : {filter: parameter.resourceFilter})};
    case "vec2": return {type: "vec2"};
    case "color": return {type: "color", alpha: true};
    case "rect":
    case "array":
    case "object":
    case "null": return {type: "json"};
  }
}

function extensionDefault(value: JsonValue | undefined): string | undefined {
  if (value === undefined) return undefined;
  return typeof value === "string" ? value : JSON.stringify(value);
}

/** Describes a .pxextension command. Extension arguments are always named. */
export function extensionCommandSchema(descriptor: ExtensionCommand): StoryCommandSchema {
  return {
    name: descriptor.id,
    family: "system",
    description: descriptor.description ?? descriptor.displayName ?? descriptor.id,
    positionals: "fixed",
    extension: true,
    parameters: descriptor.parameters.map((parameter) => {
      const fallback = extensionDefault(parameter.default);
      return {
        name: parameter.name,
        kind: extensionKind(parameter),
        ...(parameter.required === true ? {required: true} : {}),
        ...(fallback === undefined ? {} : {default: fallback}),
        ...(parameter.description === undefined ? {} : {description: parameter.description}),
      };
    }),
  };
}

/**
 * Every built-in Story command, then the commands of the given extensions.
 * The result is plain JSON, safe to serialize for an editor.
 */
export function storyCommandSchema(extensions: readonly ExtensionCommand[] = []): readonly StoryCommandSchema[] {
  return [...builtinSchema, ...extensions.map(extensionCommandSchema)];
}

export interface StoryCommandCompletion {
  readonly id: string;
  readonly parameters: readonly {readonly name: string; readonly required: boolean; readonly values?: readonly JsonValue[]}[];
}

function completionValues(kind: StoryValueKind): readonly JsonValue[] | undefined {
  switch (kind.type) {
    case "boolean": return [true, false];
    case "enum":
    case "ease":
    case "position":
    case "scene": return kind.values;
    default: return undefined;
  }
}

/** Command names and argument names for completion, derived from storyCommandSchema. */
export function storyCommandCompletions(extensions: readonly ExtensionCommand[] = []): readonly StoryCommandCompletion[] {
  return storyCommandSchema(extensions).map((schema) => ({
    id: schema.name,
    parameters: schema.parameters.map((parameter) => {
      const values = schema.extension === true
        ? extensions.find((item) => item.id === schema.name)?.parameters
          .find((item) => item.name === parameter.name)?.enum
        : completionValues(parameter.kind);
      return {name: parameter.name, required: parameter.required === true,
        ...(values === undefined ? {} : {values})};
    }),
  }));
}
