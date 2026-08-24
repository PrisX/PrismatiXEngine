export type RuntimePrimitive = null | boolean | number | string;
export type RuntimeValue = RuntimePrimitive | RuntimeValue[] | {[key: string]: RuntimeValue};

export interface ResourceRef {
  readonly id?: string;
  readonly uuid?: string;
  readonly path: string;
}

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

export interface PrismatiXEngine {
  log(...values: unknown[]): void;
  RegisterCommand<TArgs extends Record<string, RuntimeValue> = Record<string, RuntimeValue>, TResult = unknown>(id: string, handler: ScriptCommandHandler<TArgs, TResult>): void;
  RegisterAction<TArgs extends Record<string, RuntimeValue> = Record<string, RuntimeValue>>(id: string, handler: ScriptActionHandler<TArgs>): void;
  On(event: string, handler: (payload: RuntimeValue) => void | Promise<void>): void;
  Emit(event: string, payload?: RuntimeValue): void;

  GetVariable(name: string): RuntimeValue | undefined;
  SetVariable(name: string, value: RuntimeValue): void;

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
  PersistentVar(id: string): number;

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

  LoadAnimation(source: string): string;
  PlayAnimation(resourceId: string, awaitCompletion?: boolean, speed?: number): number;
  CancelAnimation(handle: number): void;
  WaitSeconds(seconds: number): Promise<void>;
  WaitAnimation(handle: number): Promise<void>;
  AwaitSeconds(seconds: number): RuntimeValue;
  AwaitAnimation(handle: number): RuntimeValue;

  GetMouseX(): number;
  GetMouseY(): number;
  GetLeftClick(): boolean;
  GetRightClick(): boolean;
  GetLogicalSize(): LogicalSize;
  DrawImage(path: string, x: number, y: number, width: number, height: number, alpha?: number): void;
  DrawAuto(path: string, displayMode: number, alpha?: number): void;
  DrawRect(x: number, y: number, width: number, height: number, r: number, g: number, b: number, a?: number): void;
  DrawRoundedRect(x: number, y: number, width: number, height: number, radius: number, r: number, g: number, b: number, a?: number): void;
  DrawText(text: string, x: number, y: number, size: number, r?: number, g?: number, b?: number, a?: number): void;
  MeasureText(text: string, size: number): LogicalSize;

  DebugPoint(source: string, line: number, locals?: Record<string, RuntimeValue>, scope?: string): Promise<void>;
}

declare global {
  // These globals are installed by the native/wasm JavaScriptHost before an
  // extension entry script is evaluated. `px` is an alias of `Engine`.
  var Engine: PrismatiXEngine;
  var px: PrismatiXEngine;
}

export interface PrismatiXContext {
  readonly raw: PrismatiXEngine;
  readonly variables: {
    get(name: string): RuntimeValue | undefined;
    set(name: string, value: RuntimeValue): void;
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
    persistentVar(id: string): number;
  };
  readonly audio: {
    playSE(path: string): void;
    playBGM(path: string, options?: {readonly loop?: boolean; readonly fadeMilliseconds?: number}): void;
    stopBGM(fadeMilliseconds?: number): void;
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
  };
  readonly animation: {
    load(source: string): string;
    play(resourceId: string, options?: {readonly awaitCompletion?: boolean; readonly speed?: number}): number;
    cancel(handle: number): void;
    wait(handle: number): Promise<void>;
  };
  readonly time: {
    wait(seconds: number): Promise<void>;
  };
  readonly events: {
    on(event: string, handler: (payload: RuntimeValue) => void | Promise<void>): void;
    emit(event: string, payload?: RuntimeValue): void;
  };
}

export function createPrismatiXContext(engine: PrismatiXEngine = globalThis.Engine): PrismatiXContext {
  return {
    raw: engine,
    variables: {
      get: (name) => engine.GetVariable(name),
      set: (name, value) => engine.SetVariable(name, value),
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
      persistentVar: (id) => engine.PersistentVar(id),
    },
    audio: {
      playSE: (path) => engine.PlaySE(path),
      playBGM: (path, options) => engine.PlayBGM(path, options?.loop, options?.fadeMilliseconds),
      stopBGM: (fadeMilliseconds) => engine.StopBGM(fadeMilliseconds),
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
    },
    animation: {
      load: (source) => engine.LoadAnimation(source),
      play: (resourceId, options) => engine.PlayAnimation(resourceId, options?.awaitCompletion, options?.speed),
      cancel: (handle) => engine.CancelAnimation(handle),
      wait: (handle) => engine.WaitAnimation(handle),
    },
    time: {
      wait: (seconds) => engine.WaitSeconds(seconds),
    },
    events: {
      on: (event, handler) => engine.On(event, handler),
      emit: (event, payload) => engine.Emit(event, payload),
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
