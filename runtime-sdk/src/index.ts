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

export interface RouteTransitionOptions {
  readonly preset?: string;
  /** Seconds. A zero duration settles on the incoming screen immediately. */
  readonly durationSeconds?: number;
}

export interface BackgroundRuleOptions {
  readonly durationMilliseconds?: number;
  readonly vague?: number;
}

export interface LayerTransformOptions {
  readonly x?: number;
  readonly y?: number;
  readonly scaleX?: number;
  readonly scaleY?: number;
  readonly rotation?: number;
  readonly alpha?: number;
  readonly z?: number;
}

export interface StageNodeTransform {
  readonly x?: number;
  readonly y?: number;
  readonly scaleX?: number;
  readonly scaleY?: number;
  /** Degrees, composed through the parent hierarchy. */
  readonly rotation?: number;
  /** Local opacity in the inclusive range 0..1. */
  readonly opacity?: number;
}

export type ParticlePreset = "rain" | "snow" | "sakura" | "dust" | "light" | "motes";
export type ParticleRange = number | readonly [minimum: number, maximum: number];
export interface ParticleCurvePoint {
  readonly time: number;
  readonly value: number;
}
export interface ParticleColorCurvePoint {
  readonly time: number;
  readonly value: readonly [r: number, g: number, b: number, a: number];
}
export interface ParticleVectorRanges {
  readonly x?: ParticleRange;
  readonly y?: ParticleRange;
}

export interface ParticleEmitterOptions {
  /** Reproducible stream id; all random sampling remains engine-owned. */
  readonly seed?: number;
  readonly rate?: number;
  readonly maxParticles?: number;
  readonly z?: number;
  readonly opacity?: number;
  readonly wind?: number;
  readonly speed?: number;
  readonly size?: number;
  readonly texture?: string;
  readonly atlas?: {
    readonly columns?: number;
    readonly rows?: number;
    readonly firstFrame?: number;
    readonly frameCount?: number;
  };
  readonly spawnShape?: "point" | "box" | "line" | "ellipse";
  /** Normalized logical Stage coordinates. */
  readonly position?: ParticleVectorRanges;
  /** Logical pixels per second. */
  readonly velocity?: ParticleVectorRanges;
  /** Logical pixels per second squared. */
  readonly acceleration?: ParticleVectorRanges;
  readonly lifetime?: ParticleRange;
  readonly rotation?: ParticleRange;
  readonly angularVelocity?: ParticleRange;
  readonly scale?: ParticleRange;
  readonly initialOpacity?: ParticleRange;
  readonly scaleOverLifetime?: readonly ParticleCurvePoint[];
  readonly opacityOverLifetime?: readonly ParticleCurvePoint[];
  readonly colorOverLifetime?: readonly ParticleColorCurvePoint[];
  readonly gravity?: number;
  readonly variation?: number;
  readonly burst?: number;
  readonly loop?: boolean;
  readonly duration?: number;
}

export type StageScreenEffect = "flash" | "fade" | "blur" | "vignette" | "color-grade";
export type CustomEffectParameterValue =
  number | readonly [number, number] |
  readonly [number, number, number, number];
export type CustomEffectNamedParameters =
  Readonly<Record<string, CustomEffectParameterValue>>;
/** Positional slots remain supported for compatibility with pre-schema scripts. */
export type CustomEffectParameters = CustomEffectNamedParameters |
  readonly (readonly number[])[];
export type ScreenEffectStatus = "unknown" | "playing" | "completed" | "stopped" | "cancelled";

export interface ScreenTextureReference {
  readonly kind: "screen-texture";
  readonly slot: "outgoing" | "incoming";
}

export interface EffectProgressReference {
  readonly kind: "effect-parameter";
  readonly name: "progress";
}

export interface EffectViewportReference {
  readonly kind: "effect-parameter";
  readonly name: "viewport";
}

export interface ScreenEffectContext {
  readonly outgoing: ScreenTextureReference;
  readonly incoming: ScreenTextureReference;
  readonly progress: EffectProgressReference;
  readonly viewport: EffectViewportReference;
}

export type ScreenEffectOperator =
  | "none" | "fade" | "crossfade" | "slide-left" | "slide-right" | "tiles";

export interface ScreenEffectPlan {
  readonly operator: ScreenEffectOperator;
  /** Native tile compositor parameters; ignored by non-tile operators. */
  readonly columns?: number;
  readonly rows?: number;
  readonly stagger?: number;
  readonly order?: "row-major" | "column-major" | "reverse";
  /** Symbolic inputs keep the plan portable; they are resolved by native code. */
  readonly outgoing?: ScreenTextureReference;
  readonly incoming?: ScreenTextureReference;
  readonly progress?: EffectProgressReference;
  readonly viewport?: EffectViewportReference;
}

export type ScreenEffectFactory = (context: ScreenEffectContext) => ScreenEffectPlan;

export interface TimerAwaitToken {
  readonly kind: "timer";
  readonly seconds: number;
}

export interface AnimationAwaitToken {
  readonly kind: "animation";
  readonly handle: number;
}

export interface ScreenEffectAwaitToken {
  readonly kind: "screen-effect";
  readonly handle: number;
}

export interface VideoAwaitToken {
  readonly kind: "video";
  readonly handle: number;
}

export interface AudioAwaitToken {
  readonly kind: "audio";
  readonly handle: number;
}

export type AudioBus = "music" | "sound-effect" | "voice" | "ambience";
export type AudioStatus =
  | "invalid" | "playing" | "paused" | "completed" | "stopped" | "failed";
export type AudioPhase = "single" | "intro" | "loop";

export interface AudioPlayOptions {
  readonly bus?: AudioBus;
  readonly loop?: boolean;
  readonly fadeMilliseconds?: number;
  /** Plays path once, then loops this resource while retaining the handle. */
  readonly loopPath?: string;
}

export interface AudioPlaybackModel {
  readonly id: number;
  readonly bus: AudioBus;
  readonly status: AudioStatus;
  readonly phase: AudioPhase;
  readonly path: string;
  readonly introPath: string;
  readonly loopPath: string;
  readonly playbackFrame: number;
  readonly loop: boolean;
}

/** Awaiting this object suspends only at the native audio wait boundary. */
export interface AudioHandle extends PromiseLike<void> {
  readonly id: number;
  pause(): boolean;
  resume(): boolean;
  stop(fadeMilliseconds?: number): boolean;
  seek(playbackFrame: number): boolean;
  status(): AudioStatus;
  playback(): AudioPlaybackModel | null;
  wait(): Promise<void>;
  token(): AudioAwaitToken;
}

export type VideoStatus = "unknown" | "playing" | "paused" | "completed" | "stopped" | "failed" | "unavailable";

export interface VideoPlayOptions {
  readonly volume?: number;
  readonly skippable?: boolean;
}

/** Awaiting this object suspends only at the native video wait boundary. */
export interface VideoHandle extends PromiseLike<void> {
  readonly id: number;
  pause(): boolean;
  resume(): boolean;
  stop(): boolean;
  skip(): boolean;
  status(): VideoStatus;
  error(): string;
  wait(): Promise<void>;
  token(): VideoAwaitToken;
}

export interface SaveSlotInfo {
  readonly slot: number;
  readonly exists: boolean;
  readonly chapter: string;
  readonly timestamp: number;
  readonly hasThumbnail: boolean;
}

/** Read-only dialogue state for authored HUDs and non-standard presentations. */
export interface DialogueModel {
  readonly active: boolean;
  readonly speaker: string;
  readonly fullText: string;
  readonly displayText: string;
  readonly voice: string;
  readonly currentChar: number;
  readonly totalChars: number;
  /** Normalized typewriter progress in the inclusive range 0..1. */
  readonly progress: number;
  readonly finished: boolean;
  readonly textColor: Color;
  readonly outlineColor: Color;
  readonly effect: string;
  readonly effectProgress: number;
}

export interface ChoiceModel {
  readonly index: number;
  readonly text: string;
  readonly target: string;
  readonly sourceId: string;
  readonly operationId: string;
}

export interface BacklogEntryModel {
  /** Stable append identity; unlike an array index it survives front eviction. */
  readonly sequence: number;
  readonly speaker: string;
  readonly text: string;
  readonly voice: string;
  readonly isChoice: boolean;
  readonly sourceId: string;
  readonly operationId: string;
}

/** Built-in settings remain defaults; unknown keys may be supplied by a host. */
export interface SettingsModel {
  readonly bgmVolume: number;
  readonly seVolume: number;
  readonly voiceVolume: number;
  readonly textSpeedMs: number;
  readonly autoWaitMs: number;
  readonly skipReadOnly: boolean;
  readonly fullscreen: boolean;
  readonly windowWidth: number;
  readonly windowHeight: number;
  readonly language: string;
  readonly textScale: number;
  readonly highContrast: boolean;
  readonly reducedMotion: boolean;
  readonly selfVoicing: boolean;
  readonly supportedLocales: readonly string[];
  readonly custom?: Readonly<Record<string, RuntimeValue>>;
}

export type ModelSubscriber<T> = (snapshot: T) => void;
export type Unsubscribe = () => void;

export type JsonValue =
  | null
  | boolean
  | number
  | string
  | JsonValue[]
  | { readonly [key: string]: JsonValue };

export interface ExtensionStateProvider<T extends JsonValue = JsonValue> {
  capture(): T;
  restore(state: T): void;
  migrate?(state: JsonValue, fromVersion: number, toVersion: number): T;
}

export type LogicalInputAction =
  | "navigate-up" | "navigate-down" | "navigate-left" | "navigate-right"
  | "accept" | "cancel" | "focus-next" | "focus-previous"
  | "advance" | "menu" | "backlog" | "toggle-auto" | "toggle-skip"
  | "toggle-ui" | "quick-save" | "quick-load" | "rollback" | "pause";

export type InputActionId = LogicalInputAction | (string & {});
export interface KeyRebindResult {
  readonly status: "applied" | "unknown-action" | "invalid-scancode" | "conflict" | "limit-reached";
  readonly conflicts: readonly string[];
}
export type InputEventType =
  | "pointer-down" | "pointer-up" | "pointer-move" | "pointer-wheel"
  | "text" | "action";
export type PointerButton = "none" | "primary" | "secondary" | "middle";
export interface InputEvent {
  readonly id: number;
  readonly frame: number;
  readonly type: InputEventType;
  readonly pointerId: number;
  readonly button: PointerButton;
  readonly x: number;
  readonly y: number;
  readonly deltaX: number;
  readonly deltaY: number;
  readonly text: string;
  readonly action: string;
}

export type RenderPhase =
  | "before-stage" | "after-stage" | "before-ui" | "after-ui";

export interface RenderTransform {
  readonly translateX?: number;
  readonly translateY?: number;
  readonly scaleX?: number;
  readonly scaleY?: number;
  readonly rotation?: number;
  readonly pivotX?: number;
  readonly pivotY?: number;
}

export interface RenderNodeBase extends RenderTransform {
  readonly phase?: RenderPhase;
  readonly order?: number;
  readonly visible?: boolean;
  /** Empty or omitted means every route. */
  readonly route?: string;
  readonly bounds: Rect;
  readonly clip?: Rect;
  readonly color?: Color;
}

export type RenderNode =
  | (RenderNodeBase & {
      readonly kind: "image";
      readonly path: string;
      /** Source pixels in the image; omitted draws the complete resource. */
      readonly source?: Rect;
    })
  | (RenderNodeBase & {readonly kind: "rect"})
  | (RenderNodeBase & {readonly kind: "rounded-rect"; readonly radius: number})
  | (RenderNodeBase & {
      readonly kind: "text";
      readonly text: string;
      readonly font: string;
      readonly fontSize: number;
    });

export type EngineAwaitToken = TimerAwaitToken | AnimationAwaitToken | ScreenEffectAwaitToken | VideoAwaitToken | AudioAwaitToken;

export type AnimationStatus =
  | "unknown" | "playing" | "paused" | "completed" | "cancelled" | "failed";

export interface TimelineMarkerEvent {
  readonly handle: number;
  readonly playbackClip: string;
  readonly clip: string;
  readonly clipName: string;
  readonly marker: string;
  readonly markerId: string;
  readonly loopIteration: number;
  readonly playbackLoopIteration: number;
  readonly payload: RuntimeValue;
}

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

  PushRoute(route: string, transition?: RouteTransitionOptions): boolean;
  ReplaceRoute(route: string, transition?: RouteTransitionOptions): boolean;
  BackRoute(transition?: RouteTransitionOptions): boolean;
  ShowModal(route: string, transition?: RouteTransitionOptions): boolean;
  CloseModal(transition?: RouteTransitionOptions): boolean;
  SetRouteTransition(outgoing: string, incoming: string, transition: RouteTransitionOptions): boolean;

  HasSeen(id: string): boolean;
  MarkSeen(id: string): void;
  ClearCount(): number;
  CGUnlocked(id: string): boolean;
  SceneUnlocked(id: string): boolean;
  UnlockCG(id: string): void;
  UnlockScene(id: string): void;

  GetDialogue(): DialogueModel;
  GetChoices(): readonly ChoiceModel[];
  AdvanceDialogue(): boolean;
  SelectChoice(index: number): boolean;
  GetBacklog(): readonly BacklogEntryModel[];
  ReplayBacklogVoice(sequence: number): boolean;
  RollbackBacklog(sequence: number): boolean;
  GetSettings(): SettingsModel;
  SetSetting(name: string, value: RuntimeValue): boolean;

  PlaySE(path: string): void;
  PlayVoice(path: string): void;
  StopVoice(): void;
  PlayBGM(path: string, loop?: boolean, fadeMilliseconds?: number): void;
  StopBGM(fadeMilliseconds?: number): void;
  SetBGMVolume(volume: number): void;
  SetSEVolume(volume: number): void;
  SetVoiceVolume(volume: number): void;
  SetAmbienceVolume(volume: number): void;
  PlayAmbience(path: string, loop?: boolean, fadeMilliseconds?: number): void;
  StopAmbience(fadeMilliseconds?: number): void;
  PlayAudio(path: string, bus?: AudioBus, loop?: boolean, fadeMilliseconds?: number, loopPath?: string): number;
  PauseAudio(handle: number): boolean;
  ResumeAudio(handle: number): boolean;
  StopAudio(handle: number, fadeMilliseconds?: number): boolean;
  SeekAudio(handle: number, playbackFrame: number): boolean;
  GetAudioStatus(handle: number): AudioStatus;
  GetAudioPlayback(handle: number): AudioPlaybackModel | null;

  Save(slot: number): boolean;
  Load(slot: number): boolean;
  Autosave(): boolean;
  DeleteSave(slot: number): boolean;
  QuerySave(slot: number): SaveSlotInfo;
  ListSaves(count?: number): readonly SaveSlotInfo[];

  PlayVideo(path: string, volume?: number, skippable?: boolean): number;
  PauseVideo(handle: number): boolean;
  ResumeVideo(handle: number): boolean;
  StopVideo(handle: number): boolean;
  SkipVideo(handle: number): boolean;
  GetVideoStatus(handle: number): VideoStatus;
  GetVideoError(handle: number): string;

  SetBackground(path: string, transition?: boolean): void;
  SetBackgroundRule(path: string, rulePath: string, durationMilliseconds?: number, vague?: number): void;
  SetCharacter(name: string, image: string, slot?: number, transition?: boolean, x?: number, y?: number, scale?: number): void;
  ClearCharacter(name: string, transition?: boolean): void;
  ClearCharacters(transition?: boolean): void;
  MoveCharacter(name: string, slot: number): void;
  SetLayer(name: string, image: string, x?: number, y?: number, scale?: number, alpha?: number, z?: number): void;
  SetLayerTransform(name: string, image: string, x?: number, y?: number, scaleX?: number, scaleY?: number, rotation?: number, alpha?: number, z?: number): void;
  ClearLayer(name: string): void;
  SetStageGroup(name: string, parent?: string): boolean;
  SetStageNodeParent(name: string, parent: string): boolean;
  SetStageNodeTransform(name: string, transform: StageNodeTransform): boolean;
  SetStageNodeOrder(name: string, z: number, order: number): boolean;
  SetStageNodeVisibility(name: string, visible: boolean): boolean;
  SetStageNodeEffect(name: string, effect: string, progress: number, parameters?: CustomEffectNamedParameters): boolean;
  ClearStageNodeEffect(name: string): void;
  RemoveStageNode(name: string): void;
  SetParticleEmitter(name: string, preset: ParticlePreset, options?: ParticleEmitterOptions): boolean;
  ClearParticleEmitter(name: string): void;
  Shake(milliseconds?: number, amplitude?: number): void;
  SetCamera(x: number, y: number, zoom: number): void;
  SetScreenEffect(effect: StageScreenEffect, amount: number): void;
  ClearScreenEffect(effect: StageScreenEffect): void;
  SetCustomEffect(effect: string, progress: number, parameters?: CustomEffectParameters): void;
  ClearCustomEffect(): void;
  Animate(target: string, properties: StageTweenSpec): boolean;

  LoadAnimation(path: string): string;
  PlayAnimation(resourceId: string, awaitCompletion?: boolean, speed?: number): number;
  CancelAnimation(handle: number): boolean;
  PauseAnimation(handle: number): boolean;
  ResumeAnimation(handle: number): boolean;
  SeekAnimation(handle: number, positionSeconds: number, apply?: boolean): boolean;
  FinishAnimation(handle: number, apply?: boolean): boolean;
  GetAnimationStatus(handle: number): AnimationStatus;
  GetAnimationError(handle: number): string;
  RegisterScreenEffect(id: string, plan: ScreenEffectPlan): boolean;
  PlayScreenEffect(id: string, durationSeconds?: number, parameters?: CustomEffectNamedParameters): number;
  StopScreenEffect(handle: number): boolean;
  CancelScreenEffect(handle: number): boolean;
  IsScreenEffectPlaying(handle: number): boolean;
  GetScreenEffectStatus(handle: number): ScreenEffectStatus;
  AwaitSeconds(seconds: number): TimerAwaitToken;
  AwaitAnimation(handle: number): AnimationAwaitToken;
  AwaitScreenEffect(handle: number): ScreenEffectAwaitToken;
  AwaitVideo(handle: number): VideoAwaitToken;
  AwaitAudio(handle: number): AudioAwaitToken;
  WaitSeconds(seconds: number): Promise<void>;
  WaitAnimation(handle: number): Promise<void>;
  WaitScreenEffect(handle: number): Promise<void>;
  WaitVideo(handle: number): Promise<void>;
  WaitAudio(handle: number): Promise<void>;

  DebugPoint(source: string, line: number, locals?: Record<string, RuntimeValue>, functionName?: string): Promise<void>;

  GetMouseX(): number;
  GetMouseY(): number;
  GetLeftClick(): boolean;
  GetRightClick(): boolean;
  IsInputActionPressed(action: LogicalInputAction): boolean;
  IsInputActionDown(action: LogicalInputAction): boolean;
  GetInputEvents(): readonly InputEvent[];
  ConsumeInputEvent(eventId: number): boolean;
  SuppressInputDefault(eventId: number): boolean;
  RegisterInputAction(action: string): boolean;
  SetInputActionContext(action: string, context: string): boolean;
  PushInputContext(context: string): boolean;
  PopInputContext(): boolean;
  GetInputContexts(): readonly string[];
  BindInputKey(action: string, scancode: number): boolean;
  GetInputKeyBindings(action: string): readonly number[];
  GetInputKeyCode(name: string): number | null;
  GetInputKeyName(scancode: number): string;
  RebindInputKey(action: string, key: string | number, allowConflict?: boolean): KeyRebindResult;
  AddInputKeyBinding(action: string, key: string | number, allowConflict?: boolean): KeyRebindResult;
  RemoveInputKeyBinding(action: string, key: string | number): boolean;
  BeginInputKeyCapture(): boolean;
  IsInputKeyCaptureActive(): boolean;
  TakeCapturedInputKey(): number | null;
  CancelInputKeyCapture(): void;
  IsNamedInputActionPressed(action: string): boolean;
  IsNamedInputActionDown(action: string): boolean;
  RegisterStateProvider<T extends JsonValue>(
    id: string,
    version: number,
    provider: ExtensionStateProvider<T>,
  ): void;

  GetLogicalSize(): LogicalSize;
  UpsertRenderNode(id: string, node: RenderNode): boolean;
  RemoveRenderNode(id: string): boolean;
  ClearRenderNodes(): void;
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
  "SetRouteTransition",
  "HasSeen",
  "MarkSeen",
  "ClearCount",
  "CGUnlocked",
  "SceneUnlocked",
  "UnlockCG",
  "UnlockScene",
  "GetDialogue",
  "GetChoices",
  "AdvanceDialogue",
  "SelectChoice",
  "GetBacklog",
  "ReplayBacklogVoice",
  "RollbackBacklog",
  "GetSettings",
  "SetSetting",
  "PlaySE",
  "PlayVoice",
  "StopVoice",
  "PlayBGM",
  "StopBGM",
  "SetBGMVolume",
  "SetSEVolume",
  "SetVoiceVolume",
  "SetAmbienceVolume",
  "PlayAmbience",
  "StopAmbience",
  "PlayAudio",
  "PauseAudio",
  "ResumeAudio",
  "StopAudio",
  "SeekAudio",
  "GetAudioStatus",
  "GetAudioPlayback",
  "Save",
  "Load",
  "Autosave",
  "DeleteSave",
  "QuerySave",
  "ListSaves",
  "PlayVideo",
  "PauseVideo",
  "ResumeVideo",
  "StopVideo",
  "SkipVideo",
  "GetVideoStatus",
  "GetVideoError",
  "SetBackground",
  "SetBackgroundRule",
  "SetCharacter",
  "ClearCharacter",
  "ClearCharacters",
  "MoveCharacter",
  "SetLayer",
  "SetLayerTransform",
  "ClearLayer",
  "SetStageGroup",
  "SetStageNodeParent",
  "SetStageNodeTransform",
  "SetStageNodeOrder",
  "SetStageNodeVisibility",
  "SetStageNodeEffect",
  "ClearStageNodeEffect",
  "RemoveStageNode",
  "SetParticleEmitter",
  "ClearParticleEmitter",
  "Shake",
  "SetCamera",
  "SetScreenEffect",
  "ClearScreenEffect",
  "SetCustomEffect",
  "ClearCustomEffect",
  "Animate",
  "LoadAnimation",
  "PlayAnimation",
  "CancelAnimation",
  "PauseAnimation",
  "ResumeAnimation",
  "SeekAnimation",
  "FinishAnimation",
  "GetAnimationStatus",
  "GetAnimationError",
  "RegisterScreenEffect",
  "PlayScreenEffect",
  "StopScreenEffect",
  "CancelScreenEffect",
  "IsScreenEffectPlaying",
  "GetScreenEffectStatus",
  "AwaitSeconds",
  "AwaitAnimation",
  "AwaitScreenEffect",
  "AwaitVideo",
  "AwaitAudio",
  "WaitSeconds",
  "WaitAnimation",
  "WaitScreenEffect",
  "WaitVideo",
  "WaitAudio",
  "DebugPoint",
  "GetMouseX",
  "GetMouseY",
  "GetLeftClick",
  "GetRightClick",
  "IsInputActionPressed",
  "IsInputActionDown",
  "GetInputEvents",
  "ConsumeInputEvent",
  "SuppressInputDefault",
  "RegisterInputAction",
  "SetInputActionContext",
  "PushInputContext",
  "PopInputContext",
  "GetInputContexts",
  "BindInputKey",
  "GetInputKeyBindings",
  "GetInputKeyCode",
  "GetInputKeyName",
  "RebindInputKey",
  "AddInputKeyBinding",
  "RemoveInputKeyBinding",
  "BeginInputKeyCapture",
  "IsInputKeyCaptureActive",
  "TakeCapturedInputKey",
  "CancelInputKeyCapture",
  "IsNamedInputActionPressed",
  "IsNamedInputActionDown",
  "RegisterStateProvider",
  "GetLogicalSize",
  "UpsertRenderNode",
  "RemoveRenderNode",
  "ClearRenderNodes",
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
    push(route: string, transition?: RouteTransitionOptions): boolean;
    replace(route: string, transition?: RouteTransitionOptions): boolean;
    back(transition?: RouteTransitionOptions): boolean;
    showModal(route: string, transition?: RouteTransitionOptions): boolean;
    closeModal(transition?: RouteTransitionOptions): boolean;
    setTransition(outgoing: string, incoming: string, transition: RouteTransitionOptions): boolean;
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
  readonly session: {
    dialogue(): DialogueModel;
    choices(): readonly ChoiceModel[];
    /** Delivers the current snapshot immediately, then only changed snapshots. */
    observeDialogue(subscriber: ModelSubscriber<DialogueModel>): Unsubscribe;
    /** Delivers the current snapshot immediately, then only changed snapshots. */
    observeChoices(subscriber: ModelSubscriber<readonly ChoiceModel[]>): Unsubscribe;
    advance(): boolean;
    selectChoice(index: number): boolean;
  };
  readonly backlog: {
    entries(): readonly BacklogEntryModel[];
    /** Delivers the current snapshot immediately, then only changed snapshots. */
    observe(subscriber: ModelSubscriber<readonly BacklogEntryModel[]>): Unsubscribe;
    replayVoice(sequence: number): boolean;
    rollback(sequence: number): boolean;
  };
  readonly settings: {
    get(): SettingsModel;
    /** Delivers the current snapshot immediately, then only changed snapshots. */
    observe(subscriber: ModelSubscriber<SettingsModel>): Unsubscribe;
    set(name: string, value: RuntimeValue): boolean;
  };
  readonly audio: {
    play(path: string, options?: AudioPlayOptions): AudioHandle;
    from(handle: number): AudioHandle;
    playSE(path: string): void;
    playVoice(path: string): void;
    stopVoice(): void;
    playBGM(path: string, options?: {readonly loop?: boolean; readonly fadeMilliseconds?: number}): void;
    stopBGM(fadeMilliseconds?: number): void;
    setBGMVolume(volume: number): void;
    setSEVolume(volume: number): void;
    setVoiceVolume(volume: number): void;
    setAmbienceVolume(volume: number): void;
    playAmbience(path: string, options?: {readonly loop?: boolean; readonly fadeMilliseconds?: number}): void;
    stopAmbience(fadeMilliseconds?: number): void;
  };
  readonly saves: {
    save(slot: number): boolean;
    load(slot: number): boolean;
    autosave(): boolean;
    delete(slot: number): boolean;
    query(slot: number): SaveSlotInfo;
    list(count?: number): readonly SaveSlotInfo[];
  };
  readonly video: {
    play(path: string, options?: VideoPlayOptions): VideoHandle;
    from(handle: number): VideoHandle;
  };
  readonly stage: {
    background(path: string, transition?: boolean): void;
    backgroundRule(path: string, rulePath: string, options?: BackgroundRuleOptions): void;
    character(name: string, image: string, options?: {readonly slot?: number; readonly transition?: boolean; readonly x?: number; readonly y?: number; readonly scale?: number}): void;
    clearCharacter(name: string, transition?: boolean): void;
    clearCharacters(transition?: boolean): void;
    moveCharacter(name: string, slot: number): void;
    layer(name: string, image: string, options?: LayerTransformOptions & {readonly scale?: number}): void;
    clearLayer(name: string): void;
    group(name: string, parent?: string): boolean;
    parent(name: string, parent: string): boolean;
    transform(name: string, transform: StageNodeTransform): boolean;
    order(name: string, z: number, order: number): boolean;
    visible(name: string, visible: boolean): boolean;
    nodeEffect(name: string, effect: string, progress: number, parameters?: CustomEffectNamedParameters): boolean;
    clearNodeEffect(name: string): void;
    remove(name: string): void;
    particles(name: string, preset: ParticlePreset, options?: ParticleEmitterOptions): boolean;
    clearParticles(name: string): void;
    shake(options?: {readonly milliseconds?: number; readonly amplitude?: number}): void;
    camera(options: {readonly x?: number; readonly y?: number; readonly zoom?: number}): void;
    screenEffect(effect: StageScreenEffect, amount: number): void;
    clearScreenEffect(effect: StageScreenEffect): void;
    customEffect(effect: string, progress: number, parameters?: CustomEffectParameters): void;
    clearCustomEffect(): void;
    animate(target: string, properties: StageTweenSpec): boolean;
  };
  readonly animation: {
    load(path: string): string;
    play(resourceId: string, options?: {readonly awaitCompletion?: boolean; readonly speed?: number}): number;
    cancel(handle: number): boolean;
    pause(handle: number): boolean;
    resume(handle: number): boolean;
    seek(handle: number, positionSeconds: number, apply?: boolean): boolean;
    finish(handle: number, apply?: boolean): boolean;
    status(handle: number): AnimationStatus;
    error(handle: number): string;
    onMarker(handler: (event: TimelineMarkerEvent) => void | Promise<void>): void;
    wait(handle: number): Promise<void>;
    token(handle: number): AnimationAwaitToken;
  };
  readonly time: {
    wait(seconds: number): Promise<void>;
    token(seconds: number): TimerAwaitToken;
  };
  readonly effects: {
    register(id: string, effect: ScreenEffectPlan | ScreenEffectFactory): boolean;
    play(id: string, durationSeconds?: number, parameters?: CustomEffectNamedParameters): number;
    stop(handle: number): boolean;
    cancel(handle: number): boolean;
    isPlaying(handle: number): boolean;
    status(handle: number): ScreenEffectStatus;
    wait(handle: number): Promise<void>;
    token(handle: number): ScreenEffectAwaitToken;
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
    actionPressed(action: LogicalInputAction): boolean;
    actionDown(action: LogicalInputAction): boolean;
    events(): readonly InputEvent[];
    consume(eventId: number): boolean;
    suppressDefault(eventId: number): boolean;
    registerAction(action: string): boolean;
    assignContext(action: string, context: string): boolean;
    pushContext(context: string): boolean;
    popContext(): boolean;
    contexts(): readonly string[];
    bindKey(action: string, scancode: number): boolean;
    keyBindings(action: string): readonly number[];
    keyCode(name: string): number | null;
    keyName(scancode: number): string;
    rebindKey(action: string, key: string | number, allowConflict?: boolean): KeyRebindResult;
    addKeyBinding(action: string, key: string | number, allowConflict?: boolean): KeyRebindResult;
    removeKeyBinding(action: string, key: string | number): boolean;
    beginKeyCapture(): boolean;
    keyCaptureActive(): boolean;
    takeCapturedKey(): number | null;
    cancelKeyCapture(): void;
    namedActionPressed(action: string): boolean;
    namedActionDown(action: string): boolean;
  };
  readonly state: {
    registerProvider<T extends JsonValue>(
      id: string,
      version: number,
      provider: ExtensionStateProvider<T>,
    ): void;
  };
  readonly renderer: {
    logicalSize(): LogicalSize;
    /** Creates or atomically replaces a host-owned retained draw node. */
    upsert(id: string, node: RenderNode): boolean;
    remove(id: string): boolean;
    clear(): void;
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

const screenEffectContext: ScreenEffectContext = Object.freeze({
  outgoing: Object.freeze({kind: "screen-texture", slot: "outgoing"}),
  incoming: Object.freeze({kind: "screen-texture", slot: "incoming"}),
  progress: Object.freeze({kind: "effect-parameter", name: "progress"}),
  viewport: Object.freeze({kind: "effect-parameter", name: "viewport"}),
});

function resolveScreenEffectPlan(effect: ScreenEffectPlan | ScreenEffectFactory): ScreenEffectPlan {
  return typeof effect === "function" ? effect(screenEffectContext) : effect;
}

function videoHandle(engine: PrismatiXEngine, id: number): VideoHandle {
  const wait = () => engine.WaitVideo(id);
  return Object.freeze({
    id,
    pause: () => engine.PauseVideo(id),
    resume: () => engine.ResumeVideo(id),
    stop: () => engine.StopVideo(id),
    skip: () => engine.SkipVideo(id),
    status: () => engine.GetVideoStatus(id),
    error: () => engine.GetVideoError(id),
    wait,
    token: () => engine.AwaitVideo(id),
    then: <TResult1 = void, TResult2 = never>(
      onfulfilled?: ((value: void) => TResult1 | PromiseLike<TResult1>) | null,
      onrejected?: ((reason: unknown) => TResult2 | PromiseLike<TResult2>) | null,
    ) => wait().then(onfulfilled, onrejected),
  });
}

function audioHandle(engine: PrismatiXEngine, id: number): AudioHandle {
  const wait = () => engine.WaitAudio(id);
  return Object.freeze({
    id,
    pause: () => engine.PauseAudio(id),
    resume: () => engine.ResumeAudio(id),
    stop: (fadeMilliseconds?: number) => engine.StopAudio(id, fadeMilliseconds),
    seek: (playbackFrame: number) => engine.SeekAudio(id, playbackFrame),
    status: () => engine.GetAudioStatus(id),
    playback: () => engine.GetAudioPlayback(id),
    wait,
    token: () => engine.AwaitAudio(id),
    then: <TResult1 = void, TResult2 = never>(
      onfulfilled?: ((value: void) => TResult1 | PromiseLike<TResult1>) | null,
      onrejected?: ((reason: unknown) => TResult2 | PromiseLike<TResult2>) | null,
    ) => wait().then(onfulfilled, onrejected),
  });
}

function observeModel<T>(
  engine: PrismatiXEngine,
  read: () => T,
  subscriber: ModelSubscriber<T>,
): Unsubscribe {
  let active = true;
  let fingerprint: string | undefined;
  const publish = () => {
    if (!active) return;
    const snapshot = read();
    const nextFingerprint = JSON.stringify(snapshot);
    if (nextFingerprint === fingerprint) return;
    fingerprint = nextFingerprint;
    subscriber(snapshot);
  };
  publish();
  engine.on("frame.update", publish);
  return () => { active = false; };
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
      push: (route, transition) => engine.PushRoute(route, transition),
      replace: (route, transition) => engine.ReplaceRoute(route, transition),
      back: (transition) => engine.BackRoute(transition),
      showModal: (route, transition) => engine.ShowModal(route, transition),
      closeModal: (transition) => engine.CloseModal(transition),
      setTransition: (outgoing, incoming, transition) => engine.SetRouteTransition(outgoing, incoming, transition),
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
    session: {
      dialogue: () => engine.GetDialogue(),
      choices: () => engine.GetChoices(),
      observeDialogue: (subscriber) => observeModel(
        engine, () => engine.GetDialogue(), subscriber,
      ),
      observeChoices: (subscriber) => observeModel(
        engine, () => engine.GetChoices(), subscriber,
      ),
      advance: () => engine.AdvanceDialogue(),
      selectChoice: (index) => engine.SelectChoice(index),
    },
    backlog: {
      entries: () => engine.GetBacklog(),
      observe: (subscriber) => observeModel(
        engine, () => engine.GetBacklog(), subscriber,
      ),
      replayVoice: (sequence) => engine.ReplayBacklogVoice(sequence),
      rollback: (sequence) => engine.RollbackBacklog(sequence),
    },
    settings: {
      get: () => engine.GetSettings(),
      observe: (subscriber) => observeModel(
        engine, () => engine.GetSettings(), subscriber,
      ),
      set: (name, value) => engine.SetSetting(name, value),
    },
    audio: {
      play: (path, options) => audioHandle(
        engine,
        engine.PlayAudio(path, options?.bus, options?.loop,
          options?.fadeMilliseconds, options?.loopPath),
      ),
      from: (handle) => audioHandle(engine, handle),
      playSE: (path) => engine.PlaySE(path),
      playVoice: (path) => engine.PlayVoice(path),
      stopVoice: () => engine.StopVoice(),
      playBGM: (path, options) => engine.PlayBGM(path, options?.loop, options?.fadeMilliseconds),
      stopBGM: (fadeMilliseconds) => engine.StopBGM(fadeMilliseconds),
      setBGMVolume: (volume) => engine.SetBGMVolume(volume),
      setSEVolume: (volume) => engine.SetSEVolume(volume),
      setVoiceVolume: (volume) => engine.SetVoiceVolume(volume),
      setAmbienceVolume: (volume) => engine.SetAmbienceVolume(volume),
      playAmbience: (path, options) => engine.PlayAmbience(path, options?.loop, options?.fadeMilliseconds),
      stopAmbience: (fadeMilliseconds) => engine.StopAmbience(fadeMilliseconds),
    },
    saves: {
      save: (slot) => engine.Save(slot),
      load: (slot) => engine.Load(slot),
      autosave: () => engine.Autosave(),
      delete: (slot) => engine.DeleteSave(slot),
      query: (slot) => engine.QuerySave(slot),
      list: (count) => engine.ListSaves(count),
    },
    video: {
      play: (path, options) => videoHandle(
        engine, engine.PlayVideo(path, options?.volume, options?.skippable),
      ),
      from: (handle) => videoHandle(engine, handle),
    },
    stage: {
      background: (path, transition) => engine.SetBackground(path, transition),
      backgroundRule: (path, rulePath, options) => engine.SetBackgroundRule(path, rulePath, options?.durationMilliseconds, options?.vague),
      character: (name, image, options) => engine.SetCharacter(name, image, options?.slot, options?.transition, options?.x, options?.y, options?.scale),
      clearCharacter: (name, transition) => engine.ClearCharacter(name, transition),
      clearCharacters: (transition) => engine.ClearCharacters(transition),
      moveCharacter: (name, slot) => engine.MoveCharacter(name, slot),
      layer: (name, image, options) => {
        if (options?.scaleX !== undefined || options?.scaleY !== undefined || options?.rotation !== undefined)
          engine.SetLayerTransform(name, image, options?.x, options?.y, options?.scaleX ?? options?.scale, options?.scaleY ?? options?.scale, options?.rotation, options?.alpha, options?.z);
        else
          engine.SetLayer(name, image, options?.x, options?.y, options?.scale, options?.alpha, options?.z);
      },
      clearLayer: (name) => engine.ClearLayer(name),
      group: (name, parent) => engine.SetStageGroup(name, parent),
      parent: (name, parent) => engine.SetStageNodeParent(name, parent),
      transform: (name, transform) => engine.SetStageNodeTransform(name, transform),
      order: (name, z, order) => engine.SetStageNodeOrder(name, z, order),
      visible: (name, visible) => engine.SetStageNodeVisibility(name, visible),
      nodeEffect: (name, effect, progress, parameters) => engine.SetStageNodeEffect(name, effect, progress, parameters),
      clearNodeEffect: (name) => engine.ClearStageNodeEffect(name),
      remove: (name) => engine.RemoveStageNode(name),
      particles: (name, preset, options) => engine.SetParticleEmitter(name, preset, options),
      clearParticles: (name) => engine.ClearParticleEmitter(name),
      shake: (options) => engine.Shake(options?.milliseconds, options?.amplitude),
      camera: (options) => engine.SetCamera(options.x ?? 0, options.y ?? 0, options.zoom ?? 1),
      screenEffect: (effect, amount) => engine.SetScreenEffect(effect, amount),
      clearScreenEffect: (effect) => engine.ClearScreenEffect(effect),
      customEffect: (effect, progress, parameters) => engine.SetCustomEffect(effect, progress, parameters),
      clearCustomEffect: () => engine.ClearCustomEffect(),
      animate: (target, properties) => engine.Animate(target, properties),
    },
    animation: {
      load: (path) => engine.LoadAnimation(path),
      play: (resourceId, options) => engine.PlayAnimation(resourceId, options?.awaitCompletion, options?.speed),
      cancel: (handle) => engine.CancelAnimation(handle),
      pause: (handle) => engine.PauseAnimation(handle),
      resume: (handle) => engine.ResumeAnimation(handle),
      seek: (handle, positionSeconds, apply) => engine.SeekAnimation(handle, positionSeconds, apply),
      finish: (handle, apply) => engine.FinishAnimation(handle, apply),
      status: (handle) => engine.GetAnimationStatus(handle),
      error: (handle) => engine.GetAnimationError(handle),
      onMarker: (handler) => engine.on(
        "timeline.marker",
        (payload) => handler(payload as unknown as TimelineMarkerEvent),
      ),
      wait: (handle) => engine.WaitAnimation(handle),
      token: (handle) => engine.AwaitAnimation(handle),
    },
    time: {
      wait: (seconds) => engine.WaitSeconds(seconds),
      token: (seconds) => engine.AwaitSeconds(seconds),
    },
    effects: {
      register: (id, effect) => engine.RegisterScreenEffect(id, resolveScreenEffectPlan(effect)),
      play: (id, durationSeconds, parameters) => engine.PlayScreenEffect(id, durationSeconds, parameters),
      stop: (handle) => engine.StopScreenEffect(handle),
      cancel: (handle) => engine.CancelScreenEffect(handle),
      isPlaying: (handle) => engine.IsScreenEffectPlaying(handle),
      status: (handle) => engine.GetScreenEffectStatus(handle),
      wait: (handle) => engine.WaitScreenEffect(handle),
      token: (handle) => engine.AwaitScreenEffect(handle),
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
      actionPressed: (action) => engine.IsInputActionPressed(action),
      actionDown: (action) => engine.IsInputActionDown(action),
      events: () => engine.GetInputEvents(),
      consume: (eventId) => engine.ConsumeInputEvent(eventId),
      suppressDefault: (eventId) => engine.SuppressInputDefault(eventId),
      registerAction: (action) => engine.RegisterInputAction(action),
      assignContext: (action, context) => engine.SetInputActionContext(action, context),
      pushContext: (context) => engine.PushInputContext(context),
      popContext: () => engine.PopInputContext(),
      contexts: () => engine.GetInputContexts(),
      bindKey: (action, scancode) => engine.BindInputKey(action, scancode),
      keyBindings: (action) => engine.GetInputKeyBindings(action),
      keyCode: (name) => engine.GetInputKeyCode(name),
      keyName: (scancode) => engine.GetInputKeyName(scancode),
      rebindKey: (action, key, allowConflict) =>
        engine.RebindInputKey(action, key, allowConflict),
      addKeyBinding: (action, key, allowConflict) =>
        engine.AddInputKeyBinding(action, key, allowConflict),
      removeKeyBinding: (action, key) => engine.RemoveInputKeyBinding(action, key),
      beginKeyCapture: () => engine.BeginInputKeyCapture(),
      keyCaptureActive: () => engine.IsInputKeyCaptureActive(),
      takeCapturedKey: () => engine.TakeCapturedInputKey(),
      cancelKeyCapture: () => engine.CancelInputKeyCapture(),
      namedActionPressed: (action) => engine.IsNamedInputActionPressed(action),
      namedActionDown: (action) => engine.IsNamedInputActionDown(action),
    },
    state: {
      registerProvider: (id, version, provider) =>
        engine.RegisterStateProvider(id, version, provider),
    },
    renderer: {
      logicalSize: () => engine.GetLogicalSize(),
      upsert: (id, node) => engine.UpsertRenderNode(id, node),
      remove: (id) => engine.RemoveRenderNode(id),
      clear: () => engine.ClearRenderNodes(),
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

/**
 * Registers a declarative full-screen transition. The factory runs once and
 * receives symbolic outgoing/incoming textures plus progress and viewport
 * parameters. Native Renderer2D resolves those symbols and performs every
 * per-frame tile/blend/slide operation without JavaScript draw calls.
 */
export function defineScreenEffect(
  id: string,
  effect: ScreenEffectPlan | ScreenEffectFactory,
  engine: PrismatiXEngine = globalThis.Engine,
): boolean {
  return engine.RegisterScreenEffect(id, resolveScreenEffectPlan(effect));
}

/** Alias that makes route-transition intent explicit. */
export const defineScreenTransition = defineScreenEffect;
