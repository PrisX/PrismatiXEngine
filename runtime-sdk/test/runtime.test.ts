import {createPrismatiXContext, defineAction, defineCommand} from "../src/index.js";
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
  HasSeen: () => false,
  MarkSeen: () => undefined,
  ClearCount: () => 0,
  CGUnlocked: () => false,
  SceneUnlocked: () => false,
  UnlockCG: () => undefined,
  UnlockScene: () => undefined,
  PlaySE: () => undefined,
  PlayBGM: (path: string) => { calls.push(`bgm:${path}`); },
  StopBGM: () => undefined,
  SetBGMVolume: (volume: number) => { calls.push(`bgm-volume:${volume}`); },
  SetSEVolume: () => undefined,
  SetVoiceVolume: () => undefined,
  SetAmbienceVolume: () => undefined,
  PlayAmbience: () => undefined,
  StopAmbience: () => undefined,
  SetBackground: (path: string) => { calls.push(`bg:${path}`); },
  SetCharacter: () => undefined,
  ClearCharacter: () => undefined,
  MoveCharacter: () => undefined,
  SetLayer: () => undefined,
  ClearLayer: () => undefined,
  Shake: () => undefined,
  Animate: (target: string) => target === "yuki",
  LoadAnimation: () => "11111111-1111-4111-8111-111111111111",
  PlayAnimation: () => 7,
  CancelAnimation: (handle: number) => handle === 7,
  AwaitSeconds: (seconds: number) => ({kind: "timer", seconds} as const),
  AwaitAnimation: (handle: number) => ({kind: "animation", handle} as const),
  WaitAnimation: async () => undefined,
  WaitSeconds: async (seconds: number) => { calls.push(`wait:${seconds}`); },
  DebugPoint: async () => undefined,
  GetMouseX: () => 12,
  GetMouseY: () => 24,
  GetLeftClick: () => true,
  GetRightClick: () => false,
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
ctx.audio.playBGM("Assets/BGM/theme.ogg");
ctx.audio.setBGMVolume(80);
ctx.stage.background("Assets/BG/classroom.webp");
if (!ctx.stage.animate("yuki", {x: 100, duration: 250, ease: "outQuad"})) throw new Error("stage animation facade is not wired");
if (!ctx.animation.cancel(ctx.animation.play(ctx.animation.load("Animations/yuki.pxanim")))) throw new Error("animation facade is not wired");
if (ctx.animation.token(7).kind !== "animation" || ctx.time.token(0.5).kind !== "timer") throw new Error("await token types are not wired");
await ctx.time.wait(0.25);
await ctx.events.emit("sample", {nested: {count: 2}, values: [true, "ok"]});
if (ctx.input.mouseX() !== 12 || ctx.input.mouseY() !== 24 || !ctx.input.leftClick() || ctx.input.rightClick()) throw new Error("input facade is not wired");
if (ctx.renderer.logicalSize().w !== 1280) throw new Error("renderer logical size is not wired");
ctx.renderer.drawAuto("Assets/CG/test.webp", ctx.displayMode.Fit);
ctx.renderer.drawRect({x: 0, y: 0, w: 10, h: 10}, {r: 1, g: 2, b: 3, a: 4});
ctx.renderer.drawRoundedRect({x: 0, y: 0, w: 10, h: 10}, 4, {r: 1, g: 2, b: 3, a: 4});
ctx.renderer.drawText("hello", 1, 2, "Resources/Fonts/NotoSansTC-Bold.ttf", 20, {r: 255, g: 255, b: 255, a: 255});
if (ctx.renderer.measureText("hello", "Resources/Fonts/NotoSansTC-Bold.ttf", 20).h !== 20) throw new Error("renderer text measurement is not wired");

const expected = [
  "var:affection:profile",
  "push:settings",
  "bgm:Assets/BGM/theme.ogg",
  "bgm-volume:80",
  "bg:Assets/BG/classroom.webp",
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
