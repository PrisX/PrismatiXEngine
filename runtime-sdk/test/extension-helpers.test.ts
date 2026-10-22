import {createPrismatiXContext, defineAction, defineCommand, isAbortError} from "../src/index.js";
import type {AbortError, CancellationSignal, EngineAwaitToken, ExtensionStateProvider, JsonValue,
  PrismatiXEngine, RuntimeValue} from "../src/index.js";

// Plain assertions: this package compiles without Node.js types, like the
// sandbox it describes.
function equal(actual: unknown, expected: unknown, message: string): void {
  if (JSON.stringify(actual) !== JSON.stringify(expected))
    throw new Error(`${message}: ${JSON.stringify(actual)} !== ${JSON.stringify(expected)}`);
}
async function test(name: string, body: () => void | Promise<void>): Promise<void> {
  try { await body(); }
  catch (error) { throw new Error(`${name}: ${String(error)}`); }
}

function fakeEngine() {
  // Like the Player: `on` returns a listener id that `off` removes natively.
  const handlers = new Map<string, {id: number; handler: (payload: RuntimeValue) => unknown}[]>();
  const providers = new Map<string, {version: number; provider: ExtensionStateProvider}>();
  const logged: string[] = [];
  const registered = new Map<string, (...values: never[]) => unknown>();
  let nextListener = 1;
  const engine = {
    log: (...values: unknown[]) => { logged.push(`info:${values.join(" ")}`); },
    on: (event: string, handler: (payload: RuntimeValue) => unknown) => {
      const id = nextListener++;
      handlers.set(event, [...handlers.get(event) ?? [], {id, handler}]);
      return id;
    },
    off: (id: number) => {
      for (const [event, list] of handlers) {
        const kept = list.filter((listener) => listener.id !== id);
        if (kept.length === list.length) continue;
        handlers.set(event, kept);
        return true;
      }
      return false;
    },
    listenerCount: (event?: string) => event === undefined
      ? [...handlers.values()].reduce((total, list) => total + list.length, 0)
      : handlers.get(event)?.length ?? 0,
    emit: async () => undefined,
    RegisterStateProvider: (id: string, version: number, provider: ExtensionStateProvider) => {
      providers.set(id, {version, provider});
    },
    RegisterCommand: (id: string, handler: (...values: never[]) => unknown) => { registered.set(id, handler); },
    RegisterAction: (id: string, handler: (...values: never[]) => unknown) => { registered.set(id, handler); },
    AwaitSeconds: (seconds: number) => ({kind: "timer", seconds}),
    GetDialogue: () => ({active: false}),
  } as unknown as PrismatiXEngine;
  const fire = async (event: string, payload: RuntimeValue) => {
    for (const {handler} of [...handlers.get(event) ?? []]) await handler(payload);
  };
  return {engine, fire, providers, logged, registered};
}

await test("events.on, once and frame.onUpdate can be stopped", async () => {
  const {engine, fire} = fakeEngine();
  const ctx = createPrismatiXContext(engine);
  const seen: string[] = [];
  const stop = ctx.events.on("save.written", ({slot}) => { seen.push(`on:${slot}`); });
  ctx.events.once("save.written", (payload) => { seen.push(`once:${payload.slot}`); });
  const stopFrame = ctx.frame.onUpdate((delta) => { seen.push(`frame:${delta}`); });
  await fire("save.written", {slot: "3"});
  await fire("save.written", {slot: "4"});
  await fire("frame.update", {delta: "0.25"});
  stop();
  stopFrame();
  await fire("save.written", {slot: "5"});
  await fire("frame.update", {delta: "0.5"});
  equal(seen, ["on:3", "once:3", "on:4", "frame:0.25"], "mismatch");
});

await test("unsubscribing removes the native listener", async () => {
  const {engine, fire} = fakeEngine();
  const ctx = createPrismatiXContext(engine);
  const stop = ctx.events.on("save.written", () => undefined);
  ctx.events.once("save.written", () => undefined);
  const stopFrame = ctx.frame.onUpdate(() => undefined);
  const stopObserving = ctx.session.observeDialogue(() => undefined);
  const stopMarkers = ctx.animation.onMarker(() => undefined);
  equal([ctx.events.listenerCount("save.written"), ctx.events.listenerCount()], [2, 5], "subscribed");
  await fire("save.written", {slot: "1"});
  equal(ctx.events.listenerCount("save.written"), 1, "once removes itself after the first delivery");
  stop();
  stop();
  stopFrame();
  stopObserving();
  stopMarkers();
  equal(ctx.events.listenerCount(), 0, "every unsubscribe reaches Engine.off");
});

await test("handlers receive an abort signal and recognise AbortError", () => {
  const {engine, registered} = fakeEngine();
  let signal: CancellationSignal | undefined;
  defineCommand("game.wait", (_args, context) => { signal = context.abortSignal; }, engine);
  defineAction("game.open", (_args, context) => { signal ??= context.abortSignal; }, engine);
  const reason = Object.assign(new Error("Story command 'game.wait' was cancelled (skip)"),
    {name: "AbortError", code: "PXJS7301", kind: "skip"}) as AbortError;
  const fakeSignal = {aborted: true, reason} as unknown as CancellationSignal;
  (registered.get("game.wait") as (args: unknown, context: unknown) => void)({}, {abortSignal: fakeSignal});
  equal(signal?.reason?.kind, "skip", "the command context carries the signal");
  equal([isAbortError(reason), isAbortError(new Error("x")), isAbortError(null)], [true, false, false],
    "isAbortError");
});

await test("state.define keeps a JSON value that saves, restores and migrates", () => {
  const {engine, providers} = fakeEngine();
  const ctx = createPrismatiXContext(engine);
  const affection = ctx.state.define("game.affection", {version: 2, initial: {score: 0},
    migrate: (saved, from) => from === 1 ? {score: Number((saved as {value?: number}).value ?? 0)} : saved as {score: number}});
  affection.update((current) => ({score: current.score + 3}));
  const registeredProvider = providers.get("game.affection");
  equal(registeredProvider?.version, 2, "mismatch");
  const captured = registeredProvider!.provider.capture();
  equal(captured, {score: 3}, "mismatch");
  affection.set({score: 10});
  registeredProvider!.provider.restore(captured as JsonValue);
  equal(affection.get(), {score: 3}, "mismatch");
  equal(registeredProvider!.provider.migrate?.({value: 7}, 1, 2), {score: 7}, "mismatch");
  affection.reset();
  equal(affection.get(), {score: 0}, "mismatch");
});

await test("log writes info through Engine.log", () => {
  const {engine, logged} = fakeEngine();
  createPrismatiXContext(engine).log.info("hello", 2);
  equal(logged, ["info:hello 2"], "mismatch");
});

await test("generator handlers yield Engine await tokens", () => {
  const {engine, registered} = fakeEngine();
  const ctx = createPrismatiXContext(engine);
  defineCommand("game.pause", function* (): Generator<EngineAwaitToken, void, unknown> {
    yield ctx.time.token(0.5);
  }, engine);
  defineAction("game.open", function* (_args, context) {
    if (!context.preview) yield ctx.time.token(0.1);
  }, engine);
  const generator = registered.get("game.pause")!() as Generator<EngineAwaitToken>;
  equal(generator.next().value, {kind: "timer", seconds: 0.5}, "mismatch");
});

