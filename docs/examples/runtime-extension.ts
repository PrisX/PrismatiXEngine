import {
  createPrismatiXContext,
  defineCommand,
  type PrismatiXEngine,
  type RuntimeValue,
} from "@prismatix/runtime";

let command: ((arguments_: {amount: number}) => number | Promise<number>) | undefined;
const variables = new Map<string, RuntimeValue>([["affection", 2]]);
const emitted: RuntimeValue[] = [];
const engine = {
  RegisterCommand: (_id: string, handler: typeof command) => { command = handler; },
  GetVariable: (name: string) => variables.get(name),
  SetVariable: (name: string, value: RuntimeValue) => { variables.set(name, value); },
  emit: async (_name: string, payload?: RuntimeValue) => {
    if (payload !== undefined) emitted.push(payload);
  },
  SetCustomEffect: () => undefined,
} as unknown as PrismatiXEngine;

const context = createPrismatiXContext(engine);
// .pxeffect names are validated by the native Player against packaged uniform
// type/range metadata; scripts do not receive textures or GPU pointers.
context.stage.customEffect("dream-tone", 0.5, {amount: 0.75});
defineCommand<{amount: number}, number>("game.raiseAffection", async ({amount}) => {
  const next = Number(context.variables.get("affection") ?? 0) + amount;
  context.variables.set("affection", next);
  await context.events.emit("affection.changed", {value: next});
  return next;
}, engine);

if (command === undefined || await command({amount: 3}) !== 5 ||
    variables.get("affection") !== 5 ||
    JSON.stringify(emitted) !== '[{"value":5}]')
  throw new Error("Runtime SDK command/event lifecycle is not wired");

process.stdout.write("Runtime SDK documentation example passed\n");
