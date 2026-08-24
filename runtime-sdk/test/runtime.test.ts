import {createPrismatiXContext, defineAction, defineCommand} from "../src/index.js";
import type {PrismatiXEngine, RuntimeValue} from "../src/index.js";

const calls: string[] = [];
const variables = new Map<string, RuntimeValue>();
let commandHandler: ((args: Record<string, RuntimeValue>) => unknown) | undefined;
let actionHandler: ((args: Record<string, RuntimeValue>, context: {preview: boolean}) => unknown) | undefined;

const engine = {
  RegisterCommand: (_id: string, handler: (args: Record<string, RuntimeValue>) => unknown) => { commandHandler = handler; },
  RegisterAction: (_id: string, handler: (args: Record<string, RuntimeValue>, context: {preview: boolean}) => unknown) => { actionHandler = handler; },
  GetVariable: (name: string) => variables.get(name),
  SetVariable: (name: string, value: RuntimeValue) => { variables.set(name, value); },
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
  PersistentVar: () => 0,
  PlaySE: () => undefined,
  PlayBGM: (path: string) => { calls.push(`bgm:${path}`); },
  StopBGM: () => undefined,
  PlayAmbience: () => undefined,
  StopAmbience: () => undefined,
  SetBackground: (path: string) => { calls.push(`bg:${path}`); },
  SetCharacter: () => undefined,
  ClearCharacter: () => undefined,
  MoveCharacter: () => undefined,
  SetLayer: () => undefined,
  ClearLayer: () => undefined,
  Shake: () => undefined,
  LoadAnimation: () => "11111111-1111-4111-8111-111111111111",
  PlayAnimation: () => 7,
  CancelAnimation: () => undefined,
  WaitAnimation: async () => undefined,
  WaitSeconds: async (seconds: number) => { calls.push(`wait:${seconds}`); },
  On: () => undefined,
  Emit: () => undefined,
} as unknown as PrismatiXEngine;

const ctx = createPrismatiXContext(engine);
ctx.variables.set("affection", 12);
if (ctx.variables.get("affection") !== 12) throw new Error("variables facade is not wired");
if (!ctx.assets.exists("Assets/ok.txt")) throw new Error("asset facade is not wired");
ctx.ui.push("settings");
ctx.audio.playBGM("Assets/BGM/theme.ogg");
ctx.stage.background("Assets/BG/classroom.webp");
await ctx.time.wait(0.25);

if (calls.join("|") !== "push:settings|bgm:Assets/BGM/theme.ogg|bg:Assets/BG/classroom.webp|wait:0.25") {
  throw new Error(`unexpected facade calls: ${calls.join("|")}`);
}

defineCommand<{amount: number}, number>("test.command", ({amount}) => amount * 2, engine);
defineAction<{enabled: boolean}>("test.action", ({enabled}, context) => {
  if (!enabled || !context.preview) throw new Error("typed action context was lost");
}, engine);

if (commandHandler?.({amount: 4}) !== 8) throw new Error("defineCommand did not register the handler");
await actionHandler?.({enabled: true}, {preview: true});
