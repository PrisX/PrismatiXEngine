import {createPrismatiXContext, defineAction, defineCommand, defineScreenTransition} from "../src/index.js";
import type {PrismatiXEngine, RuntimeValue} from "../src/index.js";

const calls: string[] = [];
const variables = new Map<string, RuntimeValue>();
let commandHandler: ((args: Record<string, RuntimeValue>) => unknown) | undefined;
let actionHandler: ((args: Record<string, RuntimeValue>, context: {preview: boolean}) => unknown) | undefined;

const engine = {
  log: () => undefined,
  RegisterCommand: (_id: string, handler: (args: Record<string, RuntimeValue>) => unknown) => { commandHandler = handler; },
  RegisterAction: (_id: string, handler: (args: Record<string, RuntimeValue>, context: {preview: boolean}) => unknown) => { actionHandler = handler; },
  on: () => undefined,
  emit: async (_event: string, payload?: RuntimeValue) => { calls.push(`emit:${JSON.stringify(payload)}`); },
  On: () => undefined,
  Emit: async () => undefined,
  GetVariable: (name: string) => variables.get(name),
  SetVariable: (name: string, value: RuntimeValue, scope?: string) => { variables.set(name, value); calls.push(`var:${name}:${scope ?? "session"}`); },
  ResourceExists: (path: string) => path === "Assets/ok.txt",
  ReadResourceText: () => "ok",
  PushRoute: (route: string) => { calls.push(`push:${route}`); return true; },
  ReplaceRoute: () => true,
  BackRoute: () => true,
  ShowModal: () => true,
  CloseModal: () => true,
  SetRouteTransition: (outgoing: string, incoming: string) => { calls.push(`route-effect:${outgoing}:${incoming}`); return true; },
  HasSeen: () => false,
  MarkSeen: () => undefined,
  ClearCount: () => 0,
  CGUnlocked: () => false,
  SceneUnlocked: () => false,
  UnlockCG: () => undefined,
  UnlockScene: () => undefined,
  PlaySE: () => undefined,
  PlayVoice: (path: string) => { calls.push(`voice:${path}`); },
  StopVoice: () => { calls.push("voice-stop"); },
  PlayBGM: (path: string) => { calls.push(`bgm:${path}`); },
  StopBGM: () => undefined,
  SetBGMVolume: (volume: number) => { calls.push(`bgm-volume:${volume}`); },
  SetSEVolume: () => undefined,
  SetVoiceVolume: () => undefined,
  SetAmbienceVolume: () => undefined,
  PlayAmbience: () => undefined,
  StopAmbience: () => undefined,
  Save: (slot: number) => { calls.push(`save:${slot}`); return true; },
  Load: (slot: number) => slot === 3,
  Autosave: () => true,
  DeleteSave: () => true,
  QuerySave: (slot: number) => ({slot, exists: true, chapter: "chapter-1", timestamp: 7, hasThumbnail: true}),
  ListSaves: () => [],
  PlayVideo: (path: string) => { calls.push(`video:${path}`); return 11; },
  PauseVideo: () => true,
  ResumeVideo: () => true,
  StopVideo: () => true,
  SkipVideo: () => true,
  GetVideoStatus: () => "completed" as const,
  GetVideoError: () => "",
  SetBackground: (path: string) => { calls.push(`bg:${path}`); },
  SetBackgroundRule: (_path: string, _rule: string, duration?: number) => { calls.push(`rule:${duration}`); },
  SetCharacter: () => undefined,
  ClearCharacter: () => undefined,
  MoveCharacter: () => undefined,
  SetLayer: () => undefined,
  SetLayerTransform: (_name: string, _image: string, _x?: number, _y?: number, scaleX?: number, scaleY?: number, rotation?: number) => { calls.push(`layer:${scaleX}:${scaleY}:${rotation}`); },
  ClearLayer: () => undefined,
  SetStageGroup: () => true,
  SetStageNodeParent: () => true,
  SetStageNodeTransform: () => true,
  SetStageNodeOrder: () => true,
  SetStageNodeVisibility: () => true,
  RemoveStageNode: () => undefined,
  SetParticleEmitter: () => true,
  ClearParticleEmitter: () => undefined,
  Shake: () => undefined,
  SetCamera: (x: number, y: number, zoom: number) => { calls.push(`camera:${x}:${y}:${zoom}`); },
  SetScreenEffect: (effect: string, amount: number) => { calls.push(`stage-effect:${effect}:${amount}`); },
  ClearScreenEffect: () => undefined,
  SetCustomEffect: (effect: string, progress: number, parameters?: unknown) => {
    calls.push(`custom-effect:${effect}:${progress}:${JSON.stringify(parameters)}`);
  },
  ClearCustomEffect: () => undefined,
  Animate: (target: string) => target === "yuki",
  LoadAnimation: () => "11111111-1111-4111-8111-111111111111",
  PlayAnimation: () => 7,
  CancelAnimation: (handle: number) => handle === 7,
  RegisterScreenEffect: (id: string, plan: {operator: string}) => { calls.push(`register-effect:${id}:${plan.operator}`); return true; },
  PlayScreenEffect: () => 9,
  StopScreenEffect: () => true,
  CancelScreenEffect: () => true,
  IsScreenEffectPlaying: () => false,
  GetScreenEffectStatus: () => "completed" as const,
  AwaitSeconds: (seconds: number) => ({kind: "timer", seconds} as const),
  AwaitAnimation: (handle: number) => ({kind: "animation", handle} as const),
  AwaitScreenEffect: (handle: number) => ({kind: "screen-effect", handle} as const),
  AwaitVideo: (handle: number) => ({kind: "video", handle} as const),
  WaitAnimation: async () => undefined,
  WaitScreenEffect: async () => undefined,
  WaitVideo: async (handle: number) => { calls.push(`video-wait:${handle}`); },
  WaitSeconds: async (seconds: number) => { calls.push(`wait:${seconds}`); },
  DebugPoint: async () => undefined,
  GetMouseX: () => 12,
  GetMouseY: () => 24,
  GetLeftClick: () => true,
  GetRightClick: () => false,
  IsInputActionPressed: (action: string) => action === "advance",
  IsInputActionDown: () => false,
  RegisterStateProvider: () => undefined,
  GetLogicalSize: () => ({w: 1280, h: 720}),
  DrawImage: () => undefined,
  DrawAuto: (_path: string, mode: number) => { calls.push(`auto:${mode}`); },
  DrawRect: () => undefined,
  DrawRoundedRect: () => undefined,
  DrawText: (_text: string, _x: number, _y: number, font: string, size: number) => { calls.push(`text:${font}:${size}`); },
  MeasureText: (_text: string, _font: string, size: number) => ({w: size * 2, h: size}),
} as unknown as PrismatiXEngine;

Object.defineProperty(globalThis, "DisplayMode", {
  configurable: true,
  value: Object.freeze({
    TopLeft: 0, TopRight: 1, BottomLeft: 2, BottomRight: 3,
    Top: 4, Bottom: 5, Left: 6, Right: 7, Center: 8,
    FitWidthBottom: 9, Fit: 10, Fill: 11,
  }),
});

const ctx = createPrismatiXContext(engine);
ctx.variables.set("affection", 12, "profile");
if (ctx.variables.get("affection") !== 12) throw new Error("variables facade is not wired");
if (!ctx.assets.exists("Assets/ok.txt")) throw new Error("asset facade is not wired");
ctx.ui.push("settings");
ctx.ui.setTransition("title", "hud", {preset: "tile", durationSeconds: 0.8});
ctx.audio.playBGM("Assets/BGM/theme.ogg");
ctx.audio.playVoice("Assets/Voice/001.ogg");
ctx.audio.stopVoice();
ctx.audio.setBGMVolume(80);
if (!ctx.saves.save(3) || !ctx.saves.load(3) || ctx.saves.query(3).chapter !== "chapter-1") throw new Error("save facade is not wired");
const video = ctx.video.play("Assets/Video/op.mp4", {volume: 0.8, skippable: true});
if (video.id !== 11 || video.status() !== "completed" || video.token().kind !== "video") throw new Error("video handle is not wired");
await video;
ctx.stage.background("Assets/BG/classroom.webp");
ctx.stage.backgroundRule("Assets/BG/night.webp", "Assets/Rules/clouds.webp", {durationMilliseconds: 700});
ctx.stage.layer("mist", "Assets/FX/mist.webp", {scaleX: 1.2, scaleY: 0.8, rotation: 12});
if (!ctx.stage.group("weather") || !ctx.stage.parent("mist", "weather") ||
    !ctx.stage.transform("weather", {x: 4, opacity: 0.8}) ||
    !ctx.stage.order("mist", -1, 2) || !ctx.stage.visible("mist", true) ||
    !ctx.stage.particles("rain", "rain", {seed: 7, maxParticles: 128}))
  throw new Error("stage graph/particle facade is not wired");
ctx.stage.camera({x: 4, y: -2, zoom: 1.1});
ctx.stage.screenEffect("fade", 0.3);
ctx.stage.customEffect("dream-tone", 0.65, {amount: 0.8, tint: [1, 0.5, 0, 1]});
if (!ctx.stage.animate("yuki", {x: 100, duration: 250, ease: "outQuad"})) throw new Error("stage animation facade is not wired");
if (!ctx.animation.cancel(ctx.animation.play(ctx.animation.load("Animations/yuki.pxanim")))) throw new Error("animation facade is not wired");
if (ctx.animation.token(7).kind !== "animation" || ctx.time.token(0.5).kind !== "timer") throw new Error("await token types are not wired");
if (!defineScreenTransition("tile", ({outgoing, incoming, progress, viewport}) => ({
  operator: "tiles", columns: 8, rows: 5, outgoing, incoming, progress, viewport,
}), engine)) throw new Error("screen transition factory is not wired");
const screenEffect = ctx.effects.play("tile", 0.8);
if (screenEffect !== 9 || ctx.effects.status(screenEffect) !== "completed" || ctx.effects.token(screenEffect).kind !== "screen-effect") throw new Error("screen effect lifecycle is not wired");
await ctx.effects.wait(screenEffect);
await ctx.time.wait(0.25);
await ctx.events.emit("sample", {nested: {count: 2}, values: [true, "ok"]});
if (ctx.input.mouseX() !== 12 || ctx.input.mouseY() !== 24 || !ctx.input.leftClick() || ctx.input.rightClick()) throw new Error("input facade is not wired");
if (!ctx.input.actionPressed("advance") || ctx.input.actionDown("pause")) throw new Error("logical input facade is not wired");
ctx.state.registerProvider("sample", 1, {
  capture: () => ({value: 1}),
  restore: () => undefined,
});
if (ctx.renderer.logicalSize().w !== 1280) throw new Error("renderer logical size is not wired");
ctx.renderer.drawAuto("Assets/CG/test.webp", ctx.displayMode.Fit);
ctx.renderer.drawRect({x: 0, y: 0, w: 10, h: 10}, {r: 1, g: 2, b: 3, a: 4});
ctx.renderer.drawRoundedRect({x: 0, y: 0, w: 10, h: 10}, 4, {r: 1, g: 2, b: 3, a: 4});
ctx.renderer.drawText("hello", 1, 2, "Resources/Fonts/NotoSansTC-Bold.ttf", 20, {r: 255, g: 255, b: 255, a: 255});
if (ctx.renderer.measureText("hello", "Resources/Fonts/NotoSansTC-Bold.ttf", 20).h !== 20) throw new Error("renderer text measurement is not wired");

const expected = [
  "var:affection:profile",
  "push:settings",
  "route-effect:title:hud",
  "bgm:Assets/BGM/theme.ogg",
  "voice:Assets/Voice/001.ogg",
  "voice-stop",
  "bgm-volume:80",
  "save:3",
  "video:Assets/Video/op.mp4",
  "video-wait:11",
  "bg:Assets/BG/classroom.webp",
  "rule:700",
  "layer:1.2:0.8:12",
  "camera:4:-2:1.1",
  "stage-effect:fade:0.3",
  'custom-effect:dream-tone:0.65:{"amount":0.8,"tint":[1,0.5,0,1]}',
  "register-effect:tile:tiles",
  "wait:0.25",
  'emit:{"nested":{"count":2},"values":[true,"ok"]}',
  "auto:10",
  "text:Resources/Fonts/NotoSansTC-Bold.ttf:20",
].join("|");
if (calls.join("|") !== expected) throw new Error(`unexpected facade calls: ${calls.join("|")}`);

defineCommand<{amount: number}, number>("test.command", ({amount}) => amount * 2, engine);
defineAction<{enabled: boolean}>("test.action", ({enabled}, context) => {
  if (!enabled || !context.preview) throw new Error("typed action context was lost");
}, engine);

if (commandHandler?.({amount: 4}) !== 8) throw new Error("defineCommand did not register the handler");
await actionHandler?.({enabled: true}, {preview: true});
