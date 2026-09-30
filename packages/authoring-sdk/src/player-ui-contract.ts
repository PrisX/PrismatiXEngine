import type {AuthoringDiagnostic, JsonValue} from "./types.js";

/**
 * What the stock Player reads from and writes into authored route UI. It
 * finds some controls by name and publishes ViewModel data under reserved
 * roots; a misspelled name or path used to fail silently at runtime.
 */
type JsonObject = Record<string, JsonValue>;

const labelTypes = ["Label", "RichTextLabel"];
// Any runtime type: the Player only checks that the control exists or shows it.
const anyType: readonly string[] = [];
const saveLoadNodes = {
  Slots: ["GridView"], SlotButton: ["Button", "TextureButton"],
  SaveLoadConfirm: anyType, SaveLoadError: anyType,
};
// Route-scoped node names the Player looks up, and the types it casts to.
// PlayerUiContractTests keeps this in step with the C++ lookups.
const namedNodes: Readonly<Record<string, Readonly<Record<string, readonly string[]>>>> = {
  hud: {
    Speaker: labelTypes, Dialogue: labelTypes, NVLText: labelTypes,
    ModeState: labelTypes, ChapterNotice: labelTypes, MusicNotice: labelTypes,
    SkipHint: labelTypes, Choices: ["VBoxContainer"],
    NoticePanel: ["EdgeRevealContainer"], EdgeToolbar: ["EdgeRevealContainer"],
    ChapterNumber: ["SpriteText"], ChapterBanner: anyType, ChoiceList: anyType,
    Face: ["TextureRect"], NVLPanel: anyType, ADVPanel: anyType, MessageWindow: anyType,
  },
  save: saveLoadNodes,
  load: saveLoadNodes,
  settings: {
    Master: ["Slider"], BGM: ["Slider"], SE: ["Slider"], Voice: ["Slider"],
    TextSpeed: ["Slider"], Fullscreen: ["CheckBox"], HighContrast: ["CheckBox"],
    ReducedMotion: ["CheckBox"], SelfVoicing: ["CheckBox"], SkipRead: ["CheckBox"],
    Language: ["OptionButton"], MuteMasterLabel: labelTypes, MuteBGMLabel: labelTypes,
    MuteSELabel: labelTypes, MuteVoiceLabel: labelTypes,
  },
  gallery: {Items: ["GridView"]},
  backlog: {Entries: ["ListView"]},
  flowchart: {scenejumpButton: ["Button", "TextureButton"], FlowSelectedItem: labelTypes,
    FlowSelectedHeading: labelTypes, FlowSelectedDetails: labelTypes},
  music: {MusicVolume: ["Slider"]},
  extra: {MusicVolume: ["Slider"]},
};
// Names the Player finds in whichever route is showing.
const anyRouteNodes: Readonly<Record<string, readonly string[]>> = {
  ConfirmDialog: anyType, DialogYes: anyType, TitleConfirm: anyType, SystemConfirm: anyType,
};
// Name prefixes the Player completes at runtime (per voice profile or slot).
const namedNodePrefixes = ["VoiceProfile_", "VoiceProfileValue_", "VoiceProfileMute_", "Slot"];

/** Every control name the stock Player looks up, for contract drift tests. */
export function playerLookupNames(): {readonly names: ReadonlySet<string>;
  readonly prefixes: readonly string[]} {
  return {names: new Set([...Object.values(namedNodes).flatMap(Object.keys),
    ...Object.keys(anyRouteNodes)]), prefixes: namedNodePrefixes};
}

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
  "cursorHideDelay", "dialogCursorWarp", "dockPinned", "messageFace"] as const;

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
    "dialogue.nvl", "dialogue.face", "dialog.visible", "dialog.skipNext",
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
    const documentRoutes = routes.get(path) ?? [];
    const contracts = [...documentRoutes.map((route) => namedNodes[route] ?? {}), anyRouteNodes];
    const hostName = (name: string) => contracts.some((contract) => contract[name] !== undefined) ||
      (documentRoutes.length > 0 && namedNodePrefixes.some((prefix) =>
        prefix !== "Slot" && name.startsWith(prefix)));
    // The Player searches the whole route for a name, and hover.<name> data
    // follows whichever control has it, so either would silently use only
    // the first match. Reusable components legitimately repeat child names
    // under different parents; siblings with one name are a mistake.
    const hovered = new Set(nodes.flatMap((node) => Object.values(object(node.bindings) ?? {}))
      .map((binding) => object(binding)?.path)
      .filter((bound): bound is string => typeof bound === "string" && bound.startsWith("hover."))
      .map((bound) => bound.slice(6)));
    const firstIndex = new Map<string, number>();
    const firstSibling = new Map<string, number>();
    nodes.forEach((node, index) => {
      if (typeof node.name !== "string" || node.name === "") return;
      const sibling = `${String(node.parentId)}\n${node.name}`;
      const first = firstIndex.get(node.name);
      const firstInParent = firstSibling.get(sibling);
      if (first === undefined) firstIndex.set(node.name, index);
      if (firstInParent === undefined) firstSibling.set(sibling, index);
      if (first === undefined) return;
      if (hostName(node.name))
        diagnostics.push({severity: "error", code: "PXBUILD1106",
          message: `The Player finds ${node.name} by name, but nodes[${first}] already uses it; only the first would work`,
          path, details: `nodes[${index}].name:${node.name}`});
      else if (firstInParent !== undefined || hovered.has(node.name))
        diagnostics.push({severity: "warning", code: "PXBUILD1107",
          message: `Control name ${node.name} is already used by nodes[${firstInParent ?? first}]` +
            (hovered.has(node.name) ? `; hover.${node.name} follows only one of them` : " in the same parent"),
          path, details: `nodes[${index}].name:${node.name}`});
    });
    if (documentRoutes.includes("hud") && nodes.length > 0 && !firstIndex.has("Dialogue"))
      diagnostics.push({severity: "warning", code: "PXBUILD1108",
        message: "The HUD has no control named Dialogue, so Story text is never shown; name the dialogue Label or RichTextLabel Dialogue",
        path});
    nodes.forEach((node, index) => {
      const name = typeof node.name === "string" ? node.name : "";
      if (name === "ConfirmDialog" && !asksAnything)
        diagnostics.push({severity: "warning", code: "PXBUILD1103",
          message: "ConfirmDialog is never shown because player.confirmations lists no operation",
          path, details: `nodes[${index}].name:${name}`});
      for (const contract of contracts) {
        const expected = contract[name];
        if (expected !== undefined && expected.length > 0 && typeof node.runtimeType === "string" &&
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
