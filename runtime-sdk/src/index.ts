export type RuntimePrimitive = null | boolean | number | string;
export type RuntimeValue = RuntimePrimitive | RuntimeValue[] | {[key: string]: RuntimeValue};

export interface ResourceRef {
  readonly id?: string;
  readonly uuid?: string;
  readonly path: string;
}

export type VariableScope = "session" | "profile";

export interface ScriptActionContext {
  readonly preview: boolean;
  readonly scene?: string;
  readonly node?: string;
  readonly signal?: string;
  readonly [key: string]: RuntimeValue | undefined;
}

export type ScriptCommandHandler<TArgs extends Record<string, RuntimeValue> = Record<string, RuntimeValue>, TResult = unknown> =
  (args: TArgs) => TResult | Promise<TResult>;

export type ScriptActionHandler<TArgs extends Record<string, RuntimeValue> = Record<string, RuntimeValue>> =
  (args: TArgs, context: ScriptActionContext) => void | Promise<void>;

export interface LogicalSize {
  readonly w: number;
  readonly h: number;
}

export interface Rect {
  readonly x: number;
  readonly y: number;
  readonly w: number;
  readonly h: number;
}

export interface Color {
  readonly r: number;
  readonly g: number;
  readonly b: number;
  readonly a: number;
}

export interface StageTweenSpec {
  readonly x?: number;
  readonly y?: number;
  readonly scale?: number;
  readonly alpha?: number;
  readonly duration?: number;
  readonly ease?: string;
}

export interface TimerAwaitToken {
  readonly kind: "timer";
  readonly seconds: number;
}

export interface AnimationAwaitToken {
  readonly kind: "animation";
  readonly handle: number;
}

export type EngineAwaitToken = TimerAwaitToken | AnimationAwaitToken;

export interface DisplayModeTable {
  readonly TopLeft: 0;
  readonly TopRight: 1;
  readonly BottomLeft: 2;
  readonly BottomRight: 3;
  readonly Top: 4;
  readonly Bottom: 5;
  readonly Left: 6;
  readonly Right: 7;
  readonly Center: 8;
  readonly FitWidthBottom: 9;
  readonly Fit: 10;
  readonly Fill: 11;
}

export type DisplayMode = DisplayModeTable[keyof DisplayModeTable];

export interface PrismatiXEngine {
  log(...values: unknown[]): void;
  RegisterCommand<TArgs extends Record<string, RuntimeValue> = Record<string, RuntimeValue>, TResult = unknown>(id: string, handler: ScriptCommandHandler<TArgs, TResult>): void;
  RegisterAction<TArgs extends Record<string, RuntimeValue> = Record<string, RuntimeValue>>(id: string, handler: ScriptActionHandler<TArgs>): void;
  on(event: string, handler: (payload: RuntimeValue) => void | Promise<void>): void;
  emit(event: string, payload?: RuntimeValue): Promise<void>;
  /** @deprecated Use on. */
  On(event: string, handler: (payload: RuntimeValue) => void | Promise<void>): void;
  /** @deprecated Use emit. */
  Emit(event: string, payload?: RuntimeValue): Promise<void>;

  GetVariable(name: string): RuntimeValue | undefined;
  SetVariable(name: string, value: RuntimeValue, scope?: VariableScope): void;

  ResourceExists(path: string): boolean;
  ReadResourceText(path: string): string;

  PushRoute(route: string): boolean;
  ReplaceRoute(route: string): boolean;
  BackRoute(): boolean;
  ShowModal(route: string): boolean;
  CloseModal(): boolean;

  HasSeen(id: string): boolean;
  MarkSeen(id: string): void;
  ClearCount(): number;
  CGUnlocked(id: string): boolean;
  SceneUnlocked(id: string): boolean;
  UnlockCG(id: string): void;
  UnlockScene(id: string): void;

  PlaySE(path: string): void;
  PlayBGM(path: string, loop?: boolean, fadeMilliseconds?: number): void;
  StopBGM(fadeMilliseconds?: number): void;
  SetBGMVolume(volume: number): void;
  SetSEVolume(volume: number): void;
  SetVoiceVolume(volume: number): void;
  SetAmbienceVolume(volume: number): void;
  PlayAmbience(path: string, loop?: boolean, fadeMilliseconds?: number): void;
  StopAmbience(fadeMilliseconds?: number): void;

  SetBackground(path: string, transition?: boolean): void;
  SetCharacter(name: string, image: string, slot?: number, transition?: boolean, x?: number, y?: number, scale?: number): void;
  ClearCharacter(name: string, transition?: boolean): void;
  MoveCharacter(name: string, slot: number): void;
  SetLayer(name: string, image: string, x?: number, y?: number, scale?: number, alpha?: number, z?: number): void;
  ClearLayer(name: string): void;
  Shake(milliseconds?: number, amplitude?: number): void;
  Animate(target: string, properties: StageTweenSpec): boolean;

  LoadAnimation(path: string): string;
  PlayAnimation(resourceId: string, awaitCompletion?: boolean, speed?: number): number;
  CancelAnimation(handle: number): boolean;
  AwaitSeconds(seconds: number): TimerAwaitToken;
  AwaitAnimation(handle: number): AnimationAwaitToken;
  WaitSeconds(seconds: number): Promise<void>;
  WaitAnimation(handle: number): Promise<void>;

  DebugPoint(source: string, line: number, locals?: Record<string, RuntimeValue>, functionName?: string): Promise<void>;

  GetMouseX(): number;
  GetMouseY(): number;
  GetLeftClick(): boolean;
  GetRightClick(): boolean;

  GetLogicalSize(): LogicalSize;
  DrawImage(path: string, x: number, y: number, width: number, height: number, alpha?: number): void;
  DrawAuto(path: string, displayMode: DisplayMode, alpha?: number): void;
  DrawRect(x: number, y: number, width: number, height: number, r: number, g: number, b: number, a: number): void;
  DrawRoundedRect(x: number, y: number, width: number, height: number, radius: number, r: number, g: number, b: number, a: number): void;
  DrawText(text: string, x: number, y: number, font: string, size: number, r: number, g: number, b: number, a?: number): void;
  MeasureText(text: string, font: string, size: number): LogicalSize;
}

export const ENGINE_HOST_BINDINGS = [
  "log",
  "RegisterCommand",
  "RegisterAction",
  "on",
  "emit",
  "On",
  "Emit",
  "GetVariable",
  "SetVariable",
  "ResourceExists",
  "ReadResourceText",
  "PushRoute",
  "ReplaceRoute",
  "BackRoute",
  "ShowModal",
  "CloseModal",
  "HasSeen",
  "MarkSeen",
  "ClearCount",
  "CGUnlocked",
  "SceneUnlocked",
  "UnlockCG",
  "UnlockScene",
  "PlaySE",
  "PlayBGM",
  "StopBGM",
  "SetBGMVolume",
  "SetSEVolume",
  "SetVoiceVolume",
  "SetAmbienceVolume",
  "PlayAmbience",
  "StopAmbience",
  "SetBackground",
  "SetCharacter",
  "ClearCharacter",
  "MoveCharacter",
  "SetLayer",
  "ClearLayer",
  "Shake",
  "Animate",
  "LoadAnimation",
  "PlayAnimation",
  "CancelAnimation",
  "AwaitSeconds",
  "AwaitAnimation",
  "WaitSeconds",
  "WaitAnimation",
  "DebugPoint",
  "GetMouseX",
  "GetMouseY",
  "GetLeftClick",
  "GetRightClick",
  "GetLogicalSize",
  "DrawImage",
  "DrawAuto",
  "DrawRect",
  "DrawRoundedRect",
  "DrawText",
  "MeasureText",
] as const satisfies readonly (keyof PrismatiXEngine)[];

type MissingEngineBinding = Exclude<keyof PrismatiXEngine, (typeof ENGINE_HOST_BINDINGS)[number]>;
const engineBindingCoverage: MissingEngineBinding extends never ? true : never = true;
void engineBindingCoverage;

declare global {
  // These globals are installed by the native/wasm JavaScriptHost before an
  // extension entry script is evaluated. `px` is an alias of `Engine`.
  var Engine: PrismatiXEngine;
  var px: PrismatiXEngine;
  var DisplayMode: DisplayModeTable;
}

export interface PrismatiXContext {
  readonly raw: PrismatiXEngine;
  readonly displayMode: DisplayModeTable;
  readonly variables: {
    get(name: string): RuntimeValue | undefined;
    set(name: string, value: RuntimeValue, scope?: VariableScope): void;
  };
  readonly assets: {
    exists(path: string): boolean;
    readText(path: string): string;
  };
  readonly ui: {
    push(route: string): boolean;
    replace(route: string): boolean;
    back(): boolean;
    showModal(route: string): boolean;
    closeModal(): boolean;
  };
  readonly progress: {
    hasSeen(id: string): boolean;
    markSeen(id: string): void;
    clearCount(): number;
    cgUnlocked(id: string): boolean;
    sceneUnlocked(id: string): boolean;
    unlockCG(id: string): void;
    unlockScene(id: string): void;
  };
  readonly audio: {
    playSE(path: string): void;
    playBGM(path: string, options?: {readonly loop?: boolean; readonly fadeMilliseconds?: number}): void;
    stopBGM(fadeMilliseconds?: number): void;
    setBGMVolume(volume: number): void;
    setSEVolume(volume: number): void;
    setVoiceVolume(volume: number): void;
    setAmbienceVolume(volume: number): void;
    playAmbience(path: string, options?: {readonly loop?: boolean; readonly fadeMilliseconds?: number}): void;
    stopAmbience(fadeMilliseconds?: number): void;
  };
  readonly stage: {
    background(path: string, transition?: boolean): void;
    character(name: string, image: string, options?: {readonly slot?: number; readonly transition?: boolean; readonly x?: number; readonly y?: number; readonly scale?: number}): void;
    clearCharacter(name: string, transition?: boolean): void;
    moveCharacter(name: string, slot: number): void;
    layer(name: string, image: string, options?: {readonly x?: number; readonly y?: number; readonly scale?: number; readonly alpha?: number; readonly z?: number}): void;
    clearLayer(name: string): void;
    shake(options?: {readonly milliseconds?: number; readonly amplitude?: number}): void;
    animate(target: string, properties: StageTweenSpec): boolean;
  };
  readonly animation: {
    load(path: string): string;
    play(resourceId: string, options?: {readonly awaitCompletion?: boolean; readonly speed?: number}): number;
    cancel(handle: number): boolean;
    wait(handle: number): Promise<void>;
    token(handle: number): AnimationAwaitToken;
  };
  readonly time: {
    wait(seconds: number): Promise<void>;
    token(seconds: number): TimerAwaitToken;
  };
  readonly events: {
    on(event: string, handler: (payload: RuntimeValue) => void | Promise<void>): void;
    emit(event: string, payload?: RuntimeValue): Promise<void>;
  };
  readonly input: {
    mouseX(): number;
    mouseY(): number;
    leftClick(): boolean;
    rightClick(): boolean;
  };
  readonly renderer: {
    logicalSize(): LogicalSize;
    drawImage(path: string, bounds: Rect, alpha?: number): void;
    drawAuto(path: string, displayMode: DisplayMode, alpha?: number): void;
    drawRect(bounds: Rect, color: Color): void;
    drawRoundedRect(bounds: Rect, radius: number, color: Color): void;
    drawText(text: string, x: number, y: number, font: string, size: number, color: Color): void;
    measureText(text: string, font: string, size: number): LogicalSize;
  };
  readonly debug: {
    point(source: string, line: number, locals?: Record<string, RuntimeValue>, functionName?: string): Promise<void>;
  };
}

const fallbackDisplayMode: DisplayModeTable = Object.freeze({
  TopLeft: 0,
  TopRight: 1,
  BottomLeft: 2,
  BottomRight: 3,
  Top: 4,
  Bottom: 5,
  Left: 6,
  Right: 7,
  Center: 8,
  FitWidthBottom: 9,
  Fit: 10,
  Fill: 11,
});

function runtimeDisplayMode(): DisplayModeTable {
  return globalThis.DisplayMode ?? fallbackDisplayMode;
}

export function createPrismatiXContext(engine: PrismatiXEngine = globalThis.Engine): PrismatiXContext {
  return {
    raw: engine,
    displayMode: runtimeDisplayMode(),
    variables: {
      get: (name) => engine.GetVariable(name),
      set: (name, value, scope) => engine.SetVariable(name, value, scope),
    },
    assets: {
      exists: (path) => engine.ResourceExists(path),
      readText: (path) => engine.ReadResourceText(path),
    },
    ui: {
      push: (route) => engine.PushRoute(route),
      replace: (route) => engine.ReplaceRoute(route),
      back: () => engine.BackRoute(),
      showModal: (route) => engine.ShowModal(route),
      closeModal: () => engine.CloseModal(),
    },
    progress: {
      hasSeen: (id) => engine.HasSeen(id),
      markSeen: (id) => engine.MarkSeen(id),
      clearCount: () => engine.ClearCount(),
      cgUnlocked: (id) => engine.CGUnlocked(id),
      sceneUnlocked: (id) => engine.SceneUnlocked(id),
      unlockCG: (id) => engine.UnlockCG(id),
      unlockScene: (id) => engine.UnlockScene(id),
    },
    audio: {
      playSE: (path) => engine.PlaySE(path),
      playBGM: (path, options) => engine.PlayBGM(path, options?.loop, options?.fadeMilliseconds),
      stopBGM: (fadeMilliseconds) => engine.StopBGM(fadeMilliseconds),
      setBGMVolume: (volume) => engine.SetBGMVolume(volume),
      setSEVolume: (volume) => engine.SetSEVolume(volume),
      setVoiceVolume: (volume) => engine.SetVoiceVolume(volume),
      setAmbienceVolume: (volume) => engine.SetAmbienceVolume(volume),
      playAmbience: (path, options) => engine.PlayAmbience(path, options?.loop, options?.fadeMilliseconds),
      stopAmbience: (fadeMilliseconds) => engine.StopAmbience(fadeMilliseconds),
    },
    stage: {
      background: (path, transition) => engine.SetBackground(path, transition),
      character: (name, image, options) => engine.SetCharacter(name, image, options?.slot, options?.transition, options?.x, options?.y, options?.scale),
      clearCharacter: (name, transition) => engine.ClearCharacter(name, transition),
      moveCharacter: (name, slot) => engine.MoveCharacter(name, slot),
      layer: (name, image, options) => engine.SetLayer(name, image, options?.x, options?.y, options?.scale, options?.alpha, options?.z),
      clearLayer: (name) => engine.ClearLayer(name),
      shake: (options) => engine.Shake(options?.milliseconds, options?.amplitude),
      animate: (target, properties) => engine.Animate(target, properties),
    },
    animation: {
      load: (path) => engine.LoadAnimation(path),
      play: (resourceId, options) => engine.PlayAnimation(resourceId, options?.awaitCompletion, options?.speed),
      cancel: (handle) => engine.CancelAnimation(handle),
      wait: (handle) => engine.WaitAnimation(handle),
      token: (handle) => engine.AwaitAnimation(handle),
    },
    time: {
      wait: (seconds) => engine.WaitSeconds(seconds),
      token: (seconds) => engine.AwaitSeconds(seconds),
    },
    events: {
      on: (event, handler) => engine.on(event, handler),
      emit: (event, payload) => engine.emit(event, payload),
    },
    input: {
      mouseX: () => engine.GetMouseX(),
      mouseY: () => engine.GetMouseY(),
      leftClick: () => engine.GetLeftClick(),
      rightClick: () => engine.GetRightClick(),
    },
    renderer: {
      logicalSize: () => engine.GetLogicalSize(),
      drawImage: (path, bounds, alpha) => engine.DrawImage(path, bounds.x, bounds.y, bounds.w, bounds.h, alpha),
      drawAuto: (path, displayMode, alpha) => engine.DrawAuto(path, displayMode, alpha),
      drawRect: (bounds, color) => engine.DrawRect(bounds.x, bounds.y, bounds.w, bounds.h, color.r, color.g, color.b, color.a),
      drawRoundedRect: (bounds, radius, color) => engine.DrawRoundedRect(bounds.x, bounds.y, bounds.w, bounds.h, radius, color.r, color.g, color.b, color.a),
      drawText: (text, x, y, font, size, color) => engine.DrawText(text, x, y, font, size, color.r, color.g, color.b, color.a),
      measureText: (text, font, size) => engine.MeasureText(text, font, size),
    },
    debug: {
      point: (source, line, locals, functionName) => engine.DebugPoint(source, line, locals, functionName),
    },
  };
}

export function defineCommand<TArgs extends Record<string, RuntimeValue>, TResult = unknown>(
  id: string,
  handler: ScriptCommandHandler<TArgs, TResult>,
  engine: PrismatiXEngine = globalThis.Engine,
): void {
  engine.RegisterCommand(id, handler);
}

export function defineAction<TArgs extends Record<string, RuntimeValue>>(
  id: string,
  handler: ScriptActionHandler<TArgs>,
  engine: PrismatiXEngine = globalThis.Engine,
): void {
  engine.RegisterAction(id, handler);
}
