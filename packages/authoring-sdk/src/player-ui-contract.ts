import type {AuthoringDiagnostic, JsonValue} from "./types.js";

/**
 * What the stock Player reads from and writes into authored route UI. It
 * finds some controls by name and publishes ViewModel data under reserved
 * roots; a misspelled name or path used to fail silently at runtime.
 */
type JsonObject = Record<string, JsonValue>;

const labelTypes = ["Label", "RichTextLabel"];
// Route-scoped node names the Player looks up, and the types it casts to.
const namedNodes: Readonly<Record<string, Readonly<Record<string, readonly string[]>>>> = {
  hud: {
    Speaker: labelTypes, Dialogue: labelTypes, NVLText: labelTypes,
    ModeState: labelTypes, ChapterNotice: labelTypes, MusicNotice: labelTypes,
    SkipHint: labelTypes, Choices: ["VBoxContainer"],
    NoticePanel: ["EdgeRevealContainer"], EdgeToolbar: ["EdgeRevealContainer"],
    ChapterNumber: ["SpriteText"],
  },
  save: {Slots: ["GridView"]},
  load: {Slots: ["GridView"]},
  settings: {
    Master: ["Slider"], BGM: ["Slider"], SE: ["Slider"], Voice: ["Slider"],
    TextSpeed: ["Slider"], Fullscreen: ["CheckBox"], HighContrast: ["CheckBox"],
    ReducedMotion: ["CheckBox"], SelfVoicing: ["CheckBox"], SkipRead: ["CheckBox"],
    Language: ["OptionButton"],
  },
  gallery: {Items: ["GridView"]},
  backlog: {Entries: ["ListView"]},
};

const itemFields: Readonly<Record<string, readonly string[]>> = {
  "saveLoad.slots": ["key", "number", "argument", "image", "occupied", "title", "subtitle", "enabled"],
  "gallery.items": ["key", "title", "image", "enabled", "locked"],
  "backlog.entries": ["key", "speaker", "text", "voice", "choice"],
  "dialogue.choices": ["key", "text", "rect"],
};

const reservedRoots = new Set(["dialogue", "chapter", "music", "saveLoad", "gallery", "backlog",
  "settings", "title", "system", "replay", "profileFlags", "musicUnlocked", "sceneUnlocked",
  "dialog", "options"]);

/** Player operations `player.confirmations` may ask before. */
export const confirmableOperations = ["quickSave", "quickLoad", "save", "overwrite", "load",
  "returnToTitle", "exit", "backlogJump", "previousChoice", "nextChoice", "resetSettings"] as const;

/** Player behaviours a `player.options` entry may bind with `binds`. */
export const optionBindings = ["textSpeed", "autoWait", "skipUnread", "messageWindowOpacity",
  "dimReadText", "uiEffects", "font", "keepSkipAfterChoice", "keepAutoAfterChoice",
  "cursorHideDelay", "dialogCursorWarp", "dockPinned"] as const;

/** Dialog kinds the project's confirmations show; each publishes `dialog.kind.<kind>`. */
export function playerDialogKinds(settings: Readonly<Record<string, JsonValue>>): string[] {
  const kinds = Object.entries(object(settings["player.confirmations"]) ?? {})
    .filter(([operation]) => (confirmableOperations as readonly string[]).includes(operation))
    .map(([operation, confirm]) => object(confirm)?.kind ?? operation)
    .filter((kind): kind is string => typeof kind === "string");
  return [...new Set(kinds)];
}

export interface PlayerUiContractInput {
  readonly uiEntryPoints: Readonly<Record<string, string>>;
  readonly settings?: Readonly<Record<string, JsonValue>>;
  /** Profile-scoped catalog variables published as `profileFlags.<name>`. */
  readonly profileVariables?: readonly string[];
}

function object(value: unknown): JsonObject | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as JsonObject : undefined;
}

function range(prefix: string, count: number): string[] {
  return Array.from({length: count}, (_, index) => `${prefix}${index + 1}`);
}

/** Every ViewModel path the stock Player publishes for this project. */
export function playerUiDataPaths(input: PlayerUiContractInput): ReadonlySet<string> {
  const settings = input.settings ?? {};
  const pages = Number(object(settings["player.saveLayout"])?.pages ?? 1);
  const tracks = Array.isArray(settings["player.musicTracks"])
    ? settings["player.musicTracks"] as JsonValue[] : [];
  const voiceProfiles = object(settings["player.voiceProfiles"])?.ids;
  const stems = tracks.filter((track): track is string => typeof track === "string")
    .map((track) => track.slice(track.lastIndexOf("/") + 1).replace(/\.[^.]+$/u, ""));
  return new Set([
    "dialogue.speaker", "dialogue.text", "dialogue.choices", "chapter.title", "music.title",
    "saveLoad.slots", "gallery.items", "backlog.entries", "title.confirm.visible",
    ...Array.from({length: 12}, (_, slot) => ["image", "title", "subtitle", "enabled"]
      .map((field) => `saveLoad.slot${slot}.${field}`)).flat(),
    ...["fullscreen", "windowed", "skipReadOnly", "skipAll", "masterValue", "bgmValue",
      "seValue", "voiceValue", "textSpeedValue", "duckOn", "duckOff",
      "voiceCarryOn", "voiceCarryOff"].map((field) => `settings.${field}`),
    ...["visible", "save", "load", "overwrite"].map((field) => `saveLoad.confirm.${field}`),
    "saveLoad.error.visible",
    ...["visible", "reset", "exit"].map((field) => `system.confirm.${field}`),
    ...range("settings.page", 4), ...range("replay.page", 4),
    ...range("saveLoad.page", Number.isSafeInteger(pages) && pages > 0 ? pages : 1),
    ...["op", "i", "ii", "iii", "iv", "v", "vi"].map((chapter) => `title.chapter.${chapter}`),
    ...range("title.chapter3Case", 9),
    ...Array.from({length: 24}, (_, index) =>
      `sceneUnlocked.scene${String(index + 1).padStart(2, "0")}`),
    ...stems.map((stem) => `musicUnlocked.${stem}`),
    ...(input.profileVariables ?? []).map((name) => `profileFlags.${name}`),
    "dialog.visible", "dialog.skipNext",
    ...playerDialogKinds(settings).map((kind) => `dialog.kind.${kind}`),
    ...["master", "bgm", "se", "voice"].map((bus) => `settings.muted.${bus}`),
    ...(Array.isArray(voiceProfiles) ? voiceProfiles : [])
      .filter((id): id is string => typeof id === "string")
      .map((id) => `settings.profileMuted.${id}`),
    ...Object.entries(object(settings["player.options"]) ?? {}).flatMap(([name, spec]) => {
      const choices = object(spec)?.choices;
      return [`options.${name}`, ...(Array.isArray(choices) ? choices : [])
        .filter((choice): choice is number => Number.isInteger(choice))
        .map((choice) => `options.${name}.is${choice}`)];
    }),
  ]);
}

function closest(value: string, candidates: Iterable<string>): string | undefined {
  let best: string | undefined;
  let bestDistance = Infinity;
  for (const candidate of candidates) {
    if (Math.abs(candidate.length - value.length) > 3) continue;
    const previous = Array.from({length: candidate.length + 1}, (_, index) => index);
    for (let row = 1; row <= value.length; row += 1) {
      let diagonal = previous[0]!;
      previous[0] = row;
      for (let column = 1; column <= candidate.length; column += 1) {
        const above = previous[column]!;
        previous[column] = Math.min(above + 1, previous[column - 1]! + 1,
          diagonal + (value[row - 1] === candidate[column - 1] ? 0 : 1));
        diagonal = above;
      }
    }
    const distance = previous[candidate.length]!;
    if (distance < bestDistance) { bestDistance = distance; best = candidate; }
  }
  return bestDistance <= 3 ? best : undefined;
}

/**
 * Checks authored route UI against the Player's host contract: reserved
 * control names must have the runtime type the Player casts to, and
 * bindings under Player-owned roots must name data the Player publishes.
 * Paths declared in `player.uiData` are the author's own and are skipped.
 */
export function validatePlayerUiContract(input: PlayerUiContractInput,
  uiDocuments: ReadonlyMap<string, JsonValue>): readonly AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  const known = playerUiDataPaths(input);
  const asksAnything = playerDialogKinds(input.settings ?? {}).length > 0;
  const authored = Object.keys(object(input.settings?.["player.uiData"]) ?? {});
  const isAuthored = (path: string) => authored.some((root) =>
    path === root || path.startsWith(`${root}.`) || path.startsWith(`${root}[`));
  const routes = new Map<string, string[]>();
  for (const [route, path] of Object.entries(input.uiEntryPoints)) {
    const compiled = path.replace(/\.[jt]sx$/u, ".pxui");
    for (const key of new Set([compiled, path]))
      routes.set(key, [...routes.get(key) ?? [], route]);
  }
  for (const [path, document] of uiDocuments) {
    const nodes = (Array.isArray(object(document)?.nodes) ? object(document)!.nodes as JsonValue[] : [])
      .map(object).filter((node): node is JsonObject => node !== undefined);
    const byId = new Map(nodes.map((node) => [node.id, node]));
    const contracts = (routes.get(path) ?? []).map((route) => namedNodes[route] ?? {});
    nodes.forEach((node, index) => {
      const name = typeof node.name === "string" ? node.name : "";
      if (name === "ConfirmDialog" && !asksAnything)
        diagnostics.push({severity: "warning", code: "PXBUILD1103",
          message: "ConfirmDialog is never shown because player.confirmations lists no operation",
          path, details: `nodes[${index}].name:${name}`});
      for (const contract of contracts) {
        const expected = contract[name];
        if (expected !== undefined && typeof node.runtimeType === "string" &&
            !expected.includes(node.runtimeType))
          // The Player only skips a control whose type does not match, so
          // this is a lost behavior rather than a broken build.
          diagnostics.push({severity: "warning", code: "PXBUILD1101",
            message: `The Player looks up ${name} as ${expected.join(" or ")} but it is ${node.runtimeType}, so the Player ignores it; rename the node if that is intended`,
            path, details: `nodes[${index}].name:${name}`});
      }
      // Item bindings resolve against the nearest templated ancestor.
      let itemSource: string | undefined;
      for (let cursor: JsonObject | undefined = node; cursor !== undefined;
        cursor = byId.get(cursor.parentId as JsonValue)) {
        const source = object(cursor.itemSource)?.path;
        if (typeof source === "string") { itemSource = source; break; }
      }
      const bindings = object(node.bindings) ?? {};
      for (const [property, binding] of Object.entries(bindings)) {
        const bound = object(binding)?.path;
        if (typeof bound !== "string") continue;
        if (bound.startsWith("item.") && itemSource !== undefined && itemFields[itemSource]) {
          const field = bound.slice(5).split(/[.[]/u)[0]!;
          if (!itemFields[itemSource]!.includes(field))
            diagnostics.push({severity: "error", code: "PXBUILD1102",
              message: `${itemSource} items have no field ${field}` +
                (closest(field, itemFields[itemSource]!) === undefined ? ""
                  : `; did you mean ${closest(field, itemFields[itemSource]!)}?`),
              path, details: `nodes[${index}].bindings.${property}:${bound}`});
          continue;
        }
        const root = bound.split(/[.[]/u)[0]!;
        if (!reservedRoots.has(root) || known.has(bound) || isAuthored(bound)) continue;
        const suggestion = closest(bound, known);
        diagnostics.push({severity: "error", code: "PXBUILD1100",
          message: `The Player publishes no UI data at ${bound}` +
            (suggestion === undefined ? "" : `; did you mean ${suggestion}?`),
          path, details: `nodes[${index}].bindings.${property}:${bound}`});
      }
      const itemPath = object(node.itemSource)?.path;
      if (typeof itemPath === "string" && reservedRoots.has(itemPath.split(".")[0]!) &&
          !known.has(itemPath) && !isAuthored(itemPath))
        diagnostics.push({severity: "error", code: "PXBUILD1100",
          message: `The Player publishes no UI collection at ${itemPath}`,
          path, details: `nodes[${index}].itemSource.path:${itemPath}`});
    });
  }
  return diagnostics;
}
