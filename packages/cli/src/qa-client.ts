// The typed client `prismatix test` hands to journey files. It speaks the QA
// IPC protocol of PrismatiXPlayerQA (newline-delimited JSON, documented in
// PrismatiXEngine/Applications/Player/QaIpc.h). Journeys import only types:
//
//   import type {PrismatixTestClient} from "@prismatix/cli/test";
//   export default async function title(px: PrismatixTestClient) {
//     await px.waitFor({route: "title", idle: true});
//     await px.screenshot("title.png");
//   }

import {connect, type Socket} from "node:net";
import {isAbsolute, resolve} from "node:path";

export type AppState = "bootSplash" | "title" | "starting" | "game";
export type VmState = "idle" | "running" | "waitingClick" | "waitingChoice" | "waitingTimer" |
  "waitingVideo" | "waitingExternal" | "paused" | "finished";
export type InputActionName = "navigate-up" | "navigate-down" | "navigate-left" | "navigate-right" |
  "accept" | "cancel" | "focus-next" | "focus-previous" | "advance" | "menu" | "backlog" |
  "toggle-auto" | "toggle-skip" | "toggle-ui" | "quick-save" | "quick-load" | "open-save" |
  "rollback" | "pause";
export type RouteOperation = "open" | "show" | "push" | "replace" | "pop" | "showModal" | "closeModal";

export interface PlayerChoice {readonly index: number; readonly text: string}

/** What the Player reports about itself; `state()` and `waitFor()` return it. */
export interface PlayerState {
  readonly appState: AppState;
  /** Base route (title, hud, ...) and the open modal route, if any. */
  readonly route: string;
  readonly modal: string;
  readonly routes: {readonly stack: readonly string[]; readonly modals: readonly string[]};
  readonly ui: {readonly route: string; readonly screen: string; readonly root: string;
    readonly animationPosition?: number};
  /** True when nothing is loading, transitioning, typing or fading. */
  readonly idle: boolean;
  /** Why the Player is not idle: routePreload, gameStart, story, typing, ... */
  readonly busy: Readonly<Record<string, true>>;
  readonly story: {
    readonly script: string; readonly vmState: VmState; readonly programCounter: number;
    readonly operationId: string; readonly label: string; readonly chapter: string;
    readonly bgm: string; readonly background: string; readonly actors: number;
    /** From the source map: the scene file and line of the current operation. */
    readonly scene?: string; readonly sceneLabel?: string;
    readonly source?: {readonly uri: string; readonly line: number} | null;
  };
  readonly dialogue: {readonly active: boolean; readonly speaker: string; readonly text: string;
    readonly displayed: string; readonly finished: boolean; readonly voice: string};
  readonly choices: readonly PlayerChoice[];
  readonly modes: {readonly auto: boolean; readonly skip: boolean; readonly hudHidden: boolean};
  /** The kind of the confirmation dialog on screen, or null. */
  readonly dialog: string | null;
  readonly locale: string;
  readonly locales?: readonly string[];
  readonly audio?: {readonly music: string; readonly voice: string; readonly mainVolume: number};
  readonly backlog?: {readonly count: number; readonly lastVoice: string};
  readonly settings?: Readonly<Record<string, unknown>>;
  readonly voiceProfiles?: readonly string[];
  readonly profile?: {readonly clearCount: number; readonly cg: readonly string[];
    readonly scenes: number; readonly music: number};
  readonly window?: {readonly width: number; readonly height: number; readonly fullscreen: boolean;
    readonly logicalWidth: number; readonly logicalHeight: number};
  readonly saveDirectory?: string;
}

/** Every field given must hold in the same frame. */
export interface WaitCondition {
  readonly appState?: AppState;
  readonly route?: string;
  readonly modal?: string;
  readonly uiRoute?: string;
  readonly screen?: string;
  readonly uiRoot?: string;
  readonly vmState?: VmState;
  readonly label?: string;
  readonly scene?: string;
  readonly sceneLabel?: string;
  /** The current Runtime IR operation id (story.operationId), or any of a list. */
  readonly operation?: string | readonly string[];
  readonly speaker?: string;
  readonly textContains?: string;
  readonly dialogueActive?: boolean;
  readonly dialog?: string | null;
  readonly locale?: string;
  readonly idle?: boolean;
  /** No script Action or command is pending. */
  readonly scriptIdle?: boolean;
  /** true: any choice is presented; a number: exactly that many. */
  readonly choices?: boolean | number;
  /** A control with this name is mounted and visible. */
  readonly control?: string;
  readonly cgUnlocked?: string;
  readonly uiAnimationStarted?: boolean;
  /** Let at least this many frames pass first. */
  readonly frames?: number;
  /** The condition must hold this many consecutive frames (default 1). */
  readonly stable?: number;
}

export interface WaitOptions {readonly timeoutMs?: number}

export interface AdvanceOptions extends WaitOptions {
  /** Fail after clicking this many lines (default 5000). */
  readonly maxLines?: number;
}

export interface FrameCapture {
  readonly width: number; readonly height: number; readonly sampledColors: number;
  readonly hash: string; readonly valid: boolean;
  /** Absolute path of the PNG, when one was written. */
  readonly path?: string;
}

export interface ControlInfo {
  readonly name: string; readonly visible: boolean; readonly enabled: boolean;
  readonly button: boolean; readonly text?: string;
}

export interface PlayerRequest {readonly op: string; readonly [field: string]: unknown}

/** The object a journey receives. Every call waits for the Player's answer. */
export interface PrismatixTestClient {
  state(): Promise<PlayerState>;
  /** Named controls of the mounted UI, optionally only names starting with prefix. */
  controls(prefix?: string): Promise<ControlInfo[]>;
  /** Activates a named control the way a click or keyboard accept does. */
  click(control: string): Promise<void>;
  /** A Player action (ui::kPlayerActions) or an authored UI/script Action. */
  action(id: string, argument?: string | number | boolean): Promise<void>;
  /** A key press, by key name ("F8", "Escape", "Return"). */
  key(name: string, options?: {readonly held?: boolean}): Promise<void>;
  /** A built-in input action. */
  input(action: InputActionName): Promise<void>;
  /** Keys bound to a named input action. */
  bindings(action: string): Promise<{readonly exists: boolean; readonly keys: readonly string[]}>;
  uiData(key: string): Promise<unknown>;
  setUiData(key: string, value: unknown): Promise<unknown>;
  variable(name: string): Promise<unknown>;
  /** Starts (from the title) or restarts the story at "scene", "scene:label" or an IR label. */
  jump(target: string): Promise<PlayerState>;
  /** A click on the text: finishes the line, or goes to the next one. */
  advance(): Promise<{readonly completedLine: boolean}>;
  choose(index: number): Promise<{readonly text: string}>;
  /**
   * Clicks through the story until the condition holds, in one round trip:
   * the Player advances a few lines (and timed waits) per frame and checks
   * the condition after each, so a matching line is never clicked past. It
   * fails at a choice the condition does not expect, and after maxLines.
   * The default timeout is 120 s.
   */
  advanceUntil(condition: WaitCondition, options?: AdvanceOptions):
    Promise<PlayerState & {readonly advancedLines: number}>;
  waitFor(condition: WaitCondition & WaitOptions, options?: WaitOptions): Promise<PlayerState>;
  /** Polls state() until the predicate holds (use waitFor for one-frame states). */
  waitUntil(predicate: (state: PlayerState) => boolean, options?: WaitOptions & {readonly message?: string}):
    Promise<PlayerState>;
  /** Captures the next composed frame; a relative path lands in the journey's output folder. */
  screenshot(path?: string, options?: {readonly native?: boolean}): Promise<FrameCapture>;
  /** Saves the current state; the save's thumbnail is the next Stage frame
   * without the UI, as the Save screen takes it. */
  save(slot: number): Promise<void>;
  load(slot: number): Promise<void>;
  /** Reads a setting, or changes it when value is given (masterVolume, textSpeedMs,
   * autoWaitMs, skipReadOnly, fullscreen, language, autoMode, skipMode, option.<name>). */
  setting(name: string, value?: unknown): Promise<unknown>;
  route(operation: RouteOperation, name?: string): Promise<{readonly stack: string[]; readonly modals: string[]}>;
  resourceExists(path: string): Promise<boolean>;
  /** Writes a line into the Player log. */
  log(message: string): Promise<void>;
  /** Runs several non-waiting requests within one frame. */
  batch(requests: readonly PlayerRequest[]): Promise<unknown[]>;
  /** Sends a raw protocol request. */
  request(op: string, fields?: Readonly<Record<string, unknown>>): Promise<unknown>;
  sleep(milliseconds: number): Promise<void>;
  quit(): Promise<void>;
  /** Where relative screenshot paths are written. */
  readonly outputDirectory: string;
}

/** What a journey module exports: `export default async function (px, context) {}`. */
export interface JourneyContext {
  readonly name: string;
  readonly file: string;
  readonly projectRoot: string;
  readonly outputDirectory: string;
  readonly dataRoot: string;
  /** Prints a line in the test report. */
  note(message: string): void;
}
export type Journey = (px: PrismatixTestClient, context: JourneyContext) => Promise<void> | void;

export class PlayerRequestError extends Error {
  constructor(readonly op: string, readonly code: string, message: string) {
    super(`${op}: ${code} ${message}`);
    this.name = "PlayerRequestError";
  }
}

const defaultWaitMs = 30_000;

export class PlayerTestClient implements PrismatixTestClient {
  private buffer = "";
  private nextId = 1;
  private closedError: Error | undefined;
  private readonly waiters = new Map<number, {
    readonly op: string;
    readonly accept: (value: unknown) => void;
    readonly reject: (error: Error) => void;
  }>();

  private constructor(private readonly socket: Socket, readonly outputDirectory: string,
    private readonly requestTimeoutMs: number) {
    socket.setEncoding("utf8");
    socket.on("data", (chunk: string) => this.receive(chunk));
    const closed = (error?: Error): void => {
      this.closedError ??= error ?? new Error("The Player closed the QA connection");
      for (const waiter of this.waiters.values()) waiter.reject(this.closedError);
      this.waiters.clear();
    };
    socket.on("error", closed);
    socket.on("close", () => closed());
  }

  static async connect(port: number, outputDirectory: string,
    requestTimeoutMs = 120_000): Promise<PlayerTestClient> {
    const socket = await new Promise<Socket>((accept, reject) => {
      const created = connect({host: "127.0.0.1", port}, () => {
        created.off("error", reject);
        accept(created);
      });
      created.once("error", reject);
    });
    socket.setNoDelay(true);
    return new PlayerTestClient(socket, outputDirectory, requestTimeoutMs);
  }

  close(): void { this.socket.destroy(); }

  private receive(chunk: string): void {
    this.buffer += chunk;
    for (let newline = this.buffer.indexOf("\n"); newline >= 0; newline = this.buffer.indexOf("\n")) {
      const line = this.buffer.slice(0, newline).trim();
      this.buffer = this.buffer.slice(newline + 1);
      if (line.length === 0) continue;
      let message: {id?: unknown; ok?: unknown; result?: unknown; error?: {code?: string; message?: string}};
      try { message = JSON.parse(line) as typeof message; } catch { continue; }
      const waiter = typeof message.id === "number" ? this.waiters.get(message.id) : undefined;
      if (waiter === undefined) continue;
      this.waiters.delete(message.id as number);
      if (message.ok === true) waiter.accept(message.result);
      else waiter.reject(new PlayerRequestError(waiter.op, message.error?.code ?? "PXQA0000",
        message.error?.message ?? "request failed"));
    }
  }

  request(op: string, fields: Readonly<Record<string, unknown>> = {}, timeoutMs?: number): Promise<unknown> {
    if (this.closedError !== undefined) return Promise.reject(this.closedError);
    const id = this.nextId++;
    const limit = timeoutMs ?? this.requestTimeoutMs;
    return new Promise((accept, reject) => {
      const timer = setTimeout(() => {
        this.waiters.delete(id);
        reject(new PlayerRequestError(op, "PXQA1101", `no answer within ${limit} ms`));
      }, limit);
      this.waiters.set(id, {op,
        accept: (value) => { clearTimeout(timer); accept(value); },
        reject: (error) => { clearTimeout(timer); reject(error); }});
      this.socket.write(`${JSON.stringify({...fields, id, op})}\n`);
    });
  }

  async state(): Promise<PlayerState> { return await this.request("state") as PlayerState; }
  async controls(prefix?: string): Promise<ControlInfo[]> {
    return await this.request("controls", prefix === undefined ? {} : {prefix}) as ControlInfo[];
  }
  async click(control: string): Promise<void> { await this.request("click", {control}); }
  async action(id: string, argument?: string | number | boolean): Promise<void> {
    await this.request("action", argument === undefined ? {action: id} : {action: id, argument: String(argument)});
  }
  async key(name: string, options: {readonly held?: boolean} = {}): Promise<void> {
    await this.request("key", {name, ...(options.held === undefined ? {} : {held: options.held})});
  }
  async input(action: InputActionName): Promise<void> { await this.request("input", {action}); }
  async bindings(action: string): Promise<{readonly exists: boolean; readonly keys: readonly string[]}> {
    return await this.request("bindings", {action}) as {exists: boolean; keys: string[]};
  }
  async uiData(key: string): Promise<unknown> { return await this.request("uiData", {key}); }
  async setUiData(key: string, value: unknown): Promise<unknown> {
    return await this.request("uiData", {key, value});
  }
  async variable(name: string): Promise<unknown> { return await this.request("variable", {name}); }
  async jump(target: string): Promise<PlayerState> {
    return await this.request("jump", {target}, 70_000) as PlayerState;
  }
  async advance(): Promise<{readonly completedLine: boolean}> {
    return await this.request("advance") as {completedLine: boolean};
  }
  async choose(index: number): Promise<{readonly text: string}> {
    return await this.request("choose", {index}) as {text: string};
  }
  async advanceUntil(condition: WaitCondition, options: AdvanceOptions = {}):
    Promise<PlayerState & {readonly advancedLines: number}> {
    const timeoutMs = options.timeoutMs ?? 120_000;
    return await this.request("advanceUntil", {condition, timeoutMs,
      ...(options.maxLines === undefined ? {} : {maxLines: options.maxLines})},
    timeoutMs + 10_000) as PlayerState & {advancedLines: number};
  }
  async waitFor(condition: WaitCondition & WaitOptions, options: WaitOptions = {}): Promise<PlayerState> {
    const {timeoutMs: inline, ...rest} = condition;
    const timeoutMs = options.timeoutMs ?? inline ?? defaultWaitMs;
    return await this.request("waitFor", {condition: rest, timeoutMs}, timeoutMs + 10_000) as PlayerState;
  }
  async waitUntil(predicate: (state: PlayerState) => boolean,
    options: WaitOptions & {readonly message?: string} = {}): Promise<PlayerState> {
    const deadline = Date.now() + (options.timeoutMs ?? defaultWaitMs);
    for (;;) {
      const state = await this.state();
      if (predicate(state)) return state;
      if (Date.now() >= deadline)
        throw new PlayerRequestError("waitUntil", "PXQA1100",
          `timed out${options.message === undefined ? "" : `: ${options.message}`} ` +
          `(appState ${state.appState}, route ${state.route}, modal ${state.modal || "-"})`);
      await this.sleep(16);
    }
  }
  async screenshot(path?: string, options: {readonly native?: boolean} = {}): Promise<FrameCapture> {
    const target = path === undefined ? undefined :
      isAbsolute(path) ? path : resolve(this.outputDirectory, path);
    return await this.request("screenshot", {
      ...(target === undefined ? {} : {path: target}),
      ...(options.native === undefined ? {} : {native: options.native})}) as FrameCapture;
  }
  async save(slot: number): Promise<void> { await this.request("save", {slot}, 20_000); }
  async load(slot: number): Promise<void> { await this.request("load", {slot}); }
  async setting(name: string, value?: unknown): Promise<unknown> {
    return await this.request("setting", value === undefined ? {name} : {name, value});
  }
  async route(operation: RouteOperation, name?: string): Promise<{readonly stack: string[]; readonly modals: string[]}> {
    return await this.request("route", name === undefined ? {operation} : {operation, name}) as
      {stack: string[]; modals: string[]};
  }
  async resourceExists(path: string): Promise<boolean> {
    return (await this.request("resource", {path}) as {exists: boolean}).exists;
  }
  async log(message: string): Promise<void> { await this.request("log", {message}); }
  async batch(requests: readonly PlayerRequest[]): Promise<unknown[]> {
    return await this.request("batch", {requests}) as unknown[];
  }
  sleep(milliseconds: number): Promise<void> {
    return new Promise((accept) => setTimeout(accept, milliseconds));
  }
  async quit(): Promise<void> { await this.request("quit", {}, 10_000); }
}
