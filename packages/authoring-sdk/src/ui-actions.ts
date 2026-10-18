import type {UiAction} from "./jsx-ui.js";
import type {JsonValue} from "./types.js";

/**
 * Typed builders for the stock Player's UI actions, so a TSX button writes
 * `action={actions.save.slot("$key")}` instead of spelling the id and
 * argument key by hand. A template placeholder such as `"$key"` or
 * `"$index"` is replaced per item when the list is built.
 */
export type ItemPlaceholder = `$${string}`;
type Slot = number | ItemPlaceholder;
export type AudioBus = "master" | "bgm" | "se" | "voice";

function action(id: string, args: Readonly<Record<string, JsonValue>> = {}): UiAction {
  return {id, arguments: args};
}

function slot(id: string, value: Slot): UiAction {
  if (typeof value === "number" && (!Number.isSafeInteger(value) || value < 0))
    throw new TypeError(`${id} needs a non-negative integer slot`);
  return action(id, {slot: value});
}

export const actions = {
  game: {start: action("game.start"), continue: action("game.continue")},
  app: {quit: action("app.quit")},
  /** Opens a Player route, or closes the top overlay. */
  screen: {
    close: action("overlay.close"), title: action("title.open"), save: action("save.open"),
    load: action("load.open"), settings: action("settings.open"), backlog: action("backlog.open"),
    gallery: action("gallery.open"), extra: action("extra.open"), replay: action("replay.open"),
    music: action("music.open"), flowchart: action("flowchart.open"),
    settingsPage: (page: number) => action(`settings.page${page}`),
    /** Any route declared in `uiEntryPoints`, such as a custom extras page. */
    open: (route: string) => action("screen.open", {argument: route}),
  },
  save: {
    quick: action("save.quick"),
    slot: (value: Slot) => slot("save.slot", value),
    page: (page: number) => action("saveLoad.page", {argument: String(page)}),
    /** Deletes the slot (asks first when `player.confirmations.deleteSave` is set). */
    delete: (value: Slot) => slot("save.delete", value),
    /** Picks the slot to copy; the next slot the player activates receives it. */
    copy: (value: Slot) => slot("save.copy", value),
    /** Picks the slot to move; moving onto another save swaps the two. */
    move: (value: Slot) => slot("save.move", value),
    /** Edits the slot's comment in the screen's `SlotComment` LineEdit. */
    comment: (value: Slot) => slot("save.comment", value),
  },
  load: {
    quick: action("load.quick"),
    slot: (value: Slot) => slot("load.slot", value),
    page: (page: number) => action("saveLoad.page", {argument: String(page)}),
  },
  saveLoad: {
    accept: action("saveLoad.confirm.accept"), cancel: action("saveLoad.confirm.cancel"),
    dismissError: action("saveLoad.error.dismiss"),
    /** Drops a copy or move that is waiting for its target slot. */
    cancelPending: action("saveLoad.pending.cancel"),
    acceptComment: action("saveLoad.comment.accept"),
    cancelComment: action("saveLoad.comment.cancel"),
  },
  choice: {
    select: (index: number | ItemPlaceholder) => action("choice.select", {index}),
    previous: action("choice.previous"), next: action("choice.next"),
  },
  mode: {auto: action("mode.auto"), skip: action("mode.skip")},
  hud: {hide: action("hud.hide"), pinToolbar: action("hud.toolbar.pin")},
  voice: {replay: action("voice.replay")},
  /**
   * A Story [input]: `accept` stores the HUD TextInput field, `cancel`
   * closes the prompt with the [input]'s default (as Escape does).
   */
  input: {accept: action("input.accept"), cancel: action("input.cancel")},
  backlog: {
    voice: (index: number | ItemPlaceholder) => action("backlog.voice", {index}),
    rollback: (index: number | ItemPlaceholder) => action("backlog.rollback", {index}),
    scroll: (rows: number) => action("backlog.scroll", {argument: String(rows)}),
  },
  gallery: {view: (resource: string | ItemPlaceholder) => action("cg.view", {resource})},
  replay: {launch: (scene: string | ItemPlaceholder) => action("replay.launch", {argument: scene})},
  /** Confirmation dialogs declared by `player.confirmations`. */
  dialog: {
    accept: action("dialog.accept"), cancel: action("dialog.cancel"),
    toggleDontAsk: action("dialog.toggleSkip"),
  },
  /** A `player.options` entry. */
  option: {
    set: (key: string, value: number) => {
      if (!/^[A-Za-z0-9_]{1,48}$/u.test(key) || !Number.isSafeInteger(value))
        throw new TypeError(`option.set needs an option name and an integer: ${key}=${value}`);
      return action("option.set", {argument: `${key}=${value}`});
    },
  },
  /** Built-in Player settings. */
  setting: {
    volume: (bus: AudioBus, value: number) => action(`set.${bus}.value`, {value}),
    volumeUp: (bus: AudioBus) => action(`set.${bus}.up`),
    volumeDown: (bus: AudioBus) => action(`set.${bus}.down`),
    toggleMute: (bus: AudioBus) => action(`set.mute.${bus}`),
    textSpeed: (value: number) => action("set.speed.value", {value}),
    fullscreen: (on: boolean) => action("set.fullscreen.value", {value: on}),
    toggleFullscreen: action("set.fullscreen.toggle"),
    skipReadOnly: (on: boolean) => action("set.skipread.value", {value: on}),
    language: (locale: string) => action("set.language.value", {locale}),
    reset: action("settings.reset"),
  },
} as const;
