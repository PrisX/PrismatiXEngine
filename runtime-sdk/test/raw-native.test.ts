import {createPrismatiXNative, encodeUtf8 as nativeEncodeUtf8, type PrismatiXNativeHost} from "../src/native.js";
import {createPrismatiXRaw, QUAD_INDICES, quadVertices, RAW_VERTEX_STRIDE, type PrismatiXRawHost} from "../src/raw.js";
import {decodeUtf8, encodeUtf8, fromBase64, fromHex, toBase64, toHex} from "../src/text.js";
import {createPrismatiXContext, type PrismatiXEngine} from "../src/index.js";

// Plain assertions: this package compiles without Node.js types.
function equal(actual: unknown, expected: unknown, message = "mismatch"): void {
  if (JSON.stringify(actual) !== JSON.stringify(expected))
    throw new Error(`${message}: ${JSON.stringify(actual)} !== ${JSON.stringify(expected)}`);
}
async function test(name: string, body: () => void | Promise<void>): Promise<void> {
  try { await body(); }
  catch (error) { throw new Error(`${name}: ${String(error)}`); }
}

function recorder<T extends object>(results: Partial<Record<string, unknown>> = {}): {host: T; calls: [string, unknown[]][]} {
  const calls: [string, unknown[]][] = [];
  const host = new Proxy({}, {get: (_target, name: string) => (...args: unknown[]) => {
    calls.push([name, args]);
    return results[name];
  }}) as T;
  return {host, calls};
}

function frameEngine(): {engine: PrismatiXEngine; tick: () => void; listeners: () => number} {
  const handlers = new Map<number, (payload: unknown) => void>();
  let next = 1;
  const engine = {
    on: (_event: string, handler: (payload: unknown) => void) => { handlers.set(next, handler); return next++; },
    off: (id: number) => handlers.delete(id),
  } as unknown as PrismatiXEngine;
  return {engine, tick: () => { for (const handler of [...handlers.values()]) handler({delta: "0.5"}); },
    listeners: () => handlers.size};
}

await test("the raw facade forwards to EngineRaw and builds quads", () => {
  const {host, calls} = recorder<PrismatiXRawHost>({DestroyMesh: true});
  const raw = createPrismatiXRaw(host);
  const mesh = raw.createMesh("fx.quad", {blend: "additive", texture: raw.asset("Assets/glow.png")});
  const vertices = quadVertices({x: 10, y: 20, w: 30, h: 40}, undefined, [1, 0.5, 0, 1]);
  mesh.vertices(vertices);
  mesh.indices(QUAD_INDICES);
  const texture = raw.createTexture("fx.noise", 8, 8, {filter: "nearest"});
  texture.upload(new Uint8Array(8 * 8 * 4));
  const target = raw.createTarget("fx.rt", 64, 64);
  raw.createMesh("fx.show", {texture: target.source, blend: "premultiplied"});
  equal(mesh.destroy(), true);
  equal(calls.map(([name]) => name), ["CreateMesh", "UploadVertices", "UploadIndices", "CreateTexture",
    "UploadTexture", "CreateTarget", "CreateMesh", "DestroyMesh"]);
  equal(calls[0]![1], ["fx.quad", {blend: "additive", texture: {kind: "asset", path: "Assets/glow.png"}}]);
  equal(calls[6]![1][1], {texture: {kind: "target", id: "fx.rt"}, blend: "premultiplied"});
  equal(vertices.length, 4 * RAW_VERTEX_STRIDE);
  equal([...vertices.slice(8, 16)], [40, 20, 1, 0, 1, 0.5, 0, 1]);
  equal([...QUAD_INDICES], [0, 1, 2, 0, 2, 3]);
});

await test("raw onFrame draws every frame until stopped", () => {
  const {engine, tick, listeners} = frameEngine();
  const previous = globalThis.Engine;
  globalThis.Engine = engine;
  try {
    const raw = createPrismatiXRaw(recorder<PrismatiXRawHost>().host);
    const deltas: number[] = [];
    const stop = raw.onFrame((delta) => deltas.push(delta));
    tick();
    stop();
    tick();
    equal(deltas, [0.5]);
    equal(listeners(), 0);
  } finally {
    globalThis.Engine = previous;
  }
});

await test("UTF-8 round-trips without TextEncoder", () => {
  equal([...encodeUtf8("é世🎉")], [0xc3, 0xa9, 0xe4, 0xb8, 0x96, 0xf0, 0x9f, 0x8e, 0x89]);
  for (const text of ["", "hello", "世界", "emoji 🎉", "é́"]) equal(decodeUtf8(encodeUtf8(text)), text);
  equal(decodeUtf8(new Uint8Array([0xff, 0x41])), "�A");
});

await test("the native facade sends bytes, text and JSON and polls messages", () => {
  const sent: [string, Uint8Array][] = [];
  const queued = [encodeUtf8("one"), encodeUtf8("two")];
  const host: PrismatiXNativeHost = {
    Services: () => ["glow.fx"],
    Has: (service) => service === "glow.fx",
    Request: (service, request) => { sent.push([service, request]); return encodeUtf8(JSON.stringify({ok: decodeUtf8(request)})); },
    TakeMessages: () => queued.splice(0),
  };
  const {engine, tick} = frameEngine();
  const previous = globalThis.Engine;
  globalThis.Engine = engine;
  try {
    const native = createPrismatiXNative(host);
    equal(native.has("glow.fx"), true);
    equal(native.requestJson("glow.fx", {n: 1}), {ok: "{\"n\":1}"});
    equal(native.requestText("glow.fx", "hi"), "{\"ok\":\"hi\"}");
    equal(sent[1]![0], "glow.fx");
    const messages: string[] = [];
    const stop = native.onMessage("glow.fx", (message) => messages.push(decodeUtf8(message)));
    tick();
    stop();
    equal(messages, ["one", "two"]);
  } finally {
    globalThis.Engine = previous;
  }
});

await test("geometry and upload batches reach EngineRaw as one call each", () => {
  const {host, calls} = recorder<PrismatiXRawHost>();
  const raw = createPrismatiXRaw(host);
  const body = raw.createMesh("live2d.body", {mask: ["live2d.clip.a", "live2d.clip.b"], maskInverted: true});
  body.geometry(quadVertices({x: 0, y: 0, w: 1, h: 1}), QUAD_INDICES);
  raw.upload([{mesh: "live2d.body", vertices: new Float32Array(8), firstVertex: 2},
    {mesh: "live2d.arm", vertices: new Float32Array(24), indices: new Uint16Array([0, 1, 2])}]);
  equal(calls.map(([name]) => name), ["CreateMesh", "UploadGeometry", "UploadBatch"]);
  equal(calls[0]![1][1], {mask: ["live2d.clip.a", "live2d.clip.b"], maskInverted: true});
  equal((calls[2]![1][0] as unknown[]).length, 2);
});

await test("text helpers: base64 (RFC 4648 vectors), hex and strict UTF-8", () => {
  const vectors: [string, string][] = [["", ""], ["f", "Zg=="], ["fo", "Zm8="], ["foo", "Zm9v"], ["foob", "Zm9vYg=="],
    ["fooba", "Zm9vYmE="], ["foobar", "Zm9vYmFy"]];
  for (const [text, encoded] of vectors) {
    equal(toBase64(encodeUtf8(text)), encoded, `toBase64(${text})`);
    equal(decodeUtf8(fromBase64(encoded)), text, `fromBase64(${encoded})`);
  }
  const bytes = new Uint8Array([251, 255, 0, 62, 63]);
  equal(toBase64(bytes), "+/8APj8=");
  equal(toBase64(bytes, true), "-_8APj8");
  equal([...fromBase64("-_8APj8")], [...bytes]);
  equal([...fromBase64(" +/8A\nPj8= ")], [...bytes]);
  equal(toHex(bytes), "fbff003e3f");
  equal([...fromHex("FBff003E3f")], [...bytes]);
  for (const bad of [() => fromBase64("Zm9v!"), () => fromBase64("Zm9vY"), () => fromHex("abc"), () => fromHex("zz")]) {
    let threw = false;
    try { bad(); } catch (error) { threw = error instanceof TypeError; }
    equal(threw, true, "malformed input throws TypeError");
  }
  equal(decodeUtf8(new Uint8Array([0xf5, 0x80, 0x80, 0x80])), "����", "0xF5 never leads");
  equal(nativeEncodeUtf8, encodeUtf8, "native re-exports the text helpers");
});

await test("ctx.random draws from the engine's saved generator", () => {
  const draws = [0, 0.999999, 0.5, 0.25, 0.75, 0.1, 0.9];
  let index = 0;
  const engine = {Random: () => draws[index++ % draws.length]!} as unknown as PrismatiXEngine;
  const random = createPrismatiXContext(engine).random;
  equal([random.int(1, 6), random.int(1, 6), random.int(1, 6)], [1, 6, 4]);
  equal(random.range(10, 20), 12.5);
  equal(random.chance(0.5), false);
  equal(random.pick(["a", "b", "c"]), "a");
  equal(random.pick([]), undefined);
  const shuffled = random.shuffle([1, 2, 3, 4]);
  equal([...shuffled].sort(), [1, 2, 3, 4], "a shuffle is a permutation");
  let threw = false;
  try { random.int(1.2, 1.8); } catch (error) { threw = error instanceof RangeError; }
  equal(threw, true, "an empty integer range throws");
});
