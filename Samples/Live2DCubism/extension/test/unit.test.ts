import assert from "node:assert/strict";
import {test} from "node:test";

import {decodeBase64} from "../src/base64.ts";
import {parseModel3, resolvePath} from "../src/model3.ts";
import {Blinker, evaluateCurve, MotionPlayer, parseExpression, parseMotion, ExpressionPlayer} from "../src/motion.ts";
import {ParameterBank} from "../src/parameters.ts";
import {
  decodeCreate, decodeUpdate, encodeUpdate, encodeUtf8, decodeUtf8, VERTEX_FLOATS,
  DYNAMIC_VISIBLE, DYNAMIC_POSITIONS_CHANGED, BLEND_ADDITIVE, BLEND_MULTIPLICATIVE, INVERTED_MASK,
  type DrawableInfo, type ModelDescription, type UpdateFrame,
} from "../src/protocol.ts";
import {MAX_RUN_VERTICES, ModelRenderer} from "../src/renderer.ts";
import {FakeRaw, MAX_UPLOADS_PER_FRAME} from "./support.ts";

test("base64 decodes the RFC 4648 vectors, ignoring whitespace and padding", () => {
  const text = (value: string) => String.fromCharCode(...decodeBase64(value));
  assert.equal(text(""), "");
  assert.equal(text("Zg=="), "f");
  assert.equal(text("Zm8="), "fo");
  assert.equal(text("Zm9v"), "foo");
  assert.equal(text("Zm9vYg=="), "foob");
  assert.equal(text("Zm9v\nYmFy\r\n"), "foobar");
  assert.equal(text("Zm9vYmE"), "fooba");
  assert.throws(() => decodeBase64("Zm9v!"), /not base64/);
});

test("UTF-8 round trips, including characters outside the BMP", () => {
  const sample = "ParamMouthOpenY 立繪 🎀";
  assert.equal(decodeUtf8(encodeUtf8(sample)), sample);
});

test("paths resolve against the model3.json folder", () => {
  assert.equal(resolvePath("Assets/Live2D/Haru", "motions/idle.motion3.json"), "Assets/Live2D/Haru/motions/idle.motion3.json");
  assert.equal(resolvePath("Assets/Live2D/Haru", "../shared/x.png"), "Assets/Live2D/shared/x.png");
  assert.equal(resolvePath("", "./a.moc3"), "a.moc3");
});

test("model3.json: moc, textures, motions, expressions and the EyeBlink/LipSync groups", () => {
  const model = parseModel3(JSON.stringify({
    Version: 3,
    FileReferences: {
      Moc: "haru.moc3", Textures: ["haru.2048/texture_00.png", "haru.2048/texture_01.png"],
      Motions: {Idle: [{File: "motions/idle.motion3.json", FadeInTime: 0.5, FadeOutTime: 0.25}], TapBody: [{File: "motions/tap.motion3.json"}]},
      Expressions: [{Name: "smile", File: "expressions/smile.exp3.json"}],
    },
    Groups: [
      {Target: "Parameter", Name: "EyeBlink", Ids: ["ParamEyeLOpen", "ParamEyeROpen"]},
      {Target: "Parameter", Name: "LipSync", Ids: ["ParamMouthOpenY"]},
      {Target: "Opacity", Name: "EyeBlink", Ids: ["ignored"]},
    ],
  }), "Assets/Live2D/Haru/haru.model3.json");
  assert.equal(model.moc, "Assets/Live2D/Haru/haru.moc3");
  assert.deepEqual(model.textures, ["Assets/Live2D/Haru/haru.2048/texture_00.png", "Assets/Live2D/Haru/haru.2048/texture_01.png"]);
  assert.deepEqual(model.motions.Idle, [{file: "Assets/Live2D/Haru/motions/idle.motion3.json", fadeIn: 0.5, fadeOut: 0.25}]);
  assert.deepEqual(model.motions.TapBody, [{file: "Assets/Live2D/Haru/motions/tap.motion3.json"}]);
  assert.equal(model.expressions.smile, "Assets/Live2D/Haru/expressions/smile.exp3.json");
  assert.deepEqual(model.eyeBlinkIds, ["ParamEyeLOpen", "ParamEyeROpen"]);
  assert.deepEqual(model.lipSyncIds, ["ParamMouthOpenY"]);
  assert.throws(() => parseModel3("{}", "x.model3.json"), /no FileReferences.Moc/);
});

const motionJson = (segments: number[]) => JSON.stringify({
  Version: 3, Meta: {Duration: 2, Loop: false},
  Curves: [{Target: "Parameter", Id: "ParamAngleX", Segments: segments}],
});

test("motion curves: linear, stepped, inverse stepped and Bezier segments", () => {
  // 0 -> 10 linear over [0,1]; stepped to 20 at t=2 (holds 10 until then); inverse stepped to 30 at t=3 (jumps at once); Bezier back to 0 at t=4.
  const motion = parseMotion(motionJson([0, 0, 0, 1, 10, 2, 2, 20, 3, 3, 30, 1, 3.33, 30, 3.66, 0, 4, 0]));
  const curve = motion.curves[0]!;
  assert.equal(evaluateCurve(curve, -1), 0);
  assert.equal(evaluateCurve(curve, 0.5), 5);
  assert.equal(evaluateCurve(curve, 1), 10);
  assert.equal(evaluateCurve(curve, 1.5), 10); // stepped holds the earlier value
  assert.equal(evaluateCurve(curve, 2.5), 30); // inverse stepped takes the later one
  const middle = evaluateCurve(curve, 3.5);
  assert.ok(middle > 0 && middle < 30, `Bezier middle ${middle}`);
  assert.ok(Math.abs(evaluateCurve(curve, 3.5) - 15) < 1.5, "a symmetric Bezier is near its midpoint at the middle");
  assert.equal(evaluateCurve(curve, 4), 0);
  assert.equal(evaluateCurve(curve, 99), 0);
  assert.throws(() => parseMotion(motionJson([0, 0, 9, 1, 1])), /unknown motion segment type 9/);
});

test("a motion fades in, holds and fades out, and a looping one does not end", () => {
  const info = [{id: "ParamAngleX", min: -30, max: 30, def: 0}];
  const bank = new ParameterBank(info, []);
  const motion = parseMotion(JSON.stringify({
    Meta: {Duration: 2, Loop: false},
    Curves: [{Target: "Parameter", Id: "ParamAngleX", Segments: [0, 20, 0, 2, 20]}],
  }));
  const player = new MotionPlayer(motion, false, 1, 1);
  const weights: number[] = [];
  for (let step = 0; step < 5; step += 1) {
    bank.reset();
    player.apply(bank, 1);
    weights.push(bank.get("ParamAngleX")! / 20);
    player.advance(0.5);
  }
  assert.equal(weights[0], 0);
  assert.ok(weights[1]! > 0 && weights[1]! < 1);
  assert.ok(Math.abs(weights[2]! - 1) < 1e-6 && Math.abs(weights[3]! - weights[1]!) < 1e-6, `weights ${weights}`);
  assert.equal(player.finished, false, "2.5 s of a 2 s motion with a 1 s fade-out is still fading");
  player.advance(1);
  assert.equal(player.finished, true);
  const looping = new MotionPlayer(motion, true, 0, 0);
  looping.advance(100);
  assert.equal(looping.finished, false);
  assert.equal(looping.controls("ParamAngleX"), true);
  assert.equal(looping.controls("ParamBreath"), false);
});

test("expressions blend Add, Multiply and Overwrite over the motion's value", () => {
  const info = ["A", "B", "C"].map((id) => ({id, min: -10, max: 10, def: 2}));
  const bank = new ParameterBank(info, []);
  const expression = parseExpression(JSON.stringify({
    FadeInTime: 0, Parameters: [{Id: "A", Value: 3, Blend: "Add"}, {Id: "B", Value: 3, Blend: "Multiply"}, {Id: "C", Value: 7, Blend: "Overwrite"}],
  }));
  new ExpressionPlayer(expression).apply(bank, 1);
  assert.deepEqual([bank.get("A"), bank.get("B"), bank.get("C")], [5, 6, 7]);
});

test("a parameter bank clamps, ignores unknown ids and starts every frame from the defaults", () => {
  const bank = new ParameterBank([{id: "P", min: 0, max: 1, def: 0.25}], ["Part"]);
  bank.set("P", 5);
  assert.equal(bank.get("P"), 1);
  bank.set("Missing", 1);
  bank.blendToward("P", 0, 0.5);
  assert.equal(bank.get("P"), 0.5);
  bank.reset();
  assert.equal(bank.get("P"), 0.25);
});

test("blinking is deterministic for a seed and closes and reopens the eyes", () => {
  const run = (seed: number) => {
    const blinker = new Blinker(["Eye"], seed);
    const bank = new ParameterBank([{id: "Eye", min: 0, max: 1, def: 1}], []);
    const seen: number[] = [];
    for (let step = 0; step < 600; step += 1) {
      blinker.advance(1 / 60);
      bank.reset();
      blinker.apply(bank, () => false);
      seen.push(bank.get("Eye")!);
    }
    return seen;
  };
  const first = run(7);
  assert.deepEqual(first, run(7));
  assert.notDeepEqual(first, run(8));
  assert.ok(first.includes(0), "the eyes close fully");
  assert.ok(first.every((value) => value >= 0 && value <= 1));
  assert.equal(first.at(-1) !== undefined, true);
  // A motion that drives the eye parameter keeps blink away from it.
  const blinker = new Blinker(["Eye"], 7);
  const bank = new ParameterBank([{id: "Eye", min: 0, max: 1, def: 1}], []);
  for (let step = 0; step < 600; step += 1) {
    blinker.advance(1 / 60);
    bank.reset();
    blinker.apply(bank, () => true);
    assert.equal(bank.get("Eye"), 1);
  }
});

// ---- wire format ---------------------------------------------------------

function description(drawables: Partial<DrawableInfo>[], vertexCount = 4): ModelDescription {
  const filled = drawables.map((drawable, at) => ({id: `D${at}`, texture: 0, flags: 0, vc: vertexCount, ic: 6, masks: [], ...drawable}));
  return {
    coreVersion: 1, canvas: {w: 100, h: 200, ox: 50, oy: 100, ppu: 100},
    parameters: [{id: "P", min: 0, max: 1, def: 0}], parts: ["Part"],
    drawables: filled, indices: filled.map((drawable) => Uint16Array.from([0, 1, 2, 0, 2, 3].slice(0, drawable.ic))),
  };
}

test("a status other than 0 becomes an Error carrying the native message", () => {
  const message = encodeUtf8("cannot load the Cubism Core library");
  const bytes = new Uint8Array(4 + message.length);
  new DataView(bytes.buffer).setUint32(0, 1, true);
  bytes.set(message, 4);
  assert.throws(() => decodeCreate(bytes), /Live2D: cannot load the Cubism Core library/);
  assert.throws(() => decodeCreate(new Uint8Array(2)), /fewer than 4 bytes/);
});

test("decodeCreate reads the description and the per-drawable indices", () => {
  const json = encodeUtf8(JSON.stringify({
    coreVersion: 3, canvas: {w: 1, h: 2, ox: 3, oy: 4, ppu: 5}, parameters: [], parts: [],
    drawables: [{id: "A", texture: 0, flags: 0, vc: 3, ic: 3, masks: []}, {id: "B", texture: 1, flags: 1, vc: 3, ic: 3, masks: [0]}],
  }));
  const padded = (json.length + 3) & ~3;
  const bytes = new Uint8Array(12 + padded + 12);
  const view = new DataView(bytes.buffer);
  view.setUint32(4, 7, true);
  view.setUint32(8, json.length, true);
  bytes.set(json, 12);
  [0, 1, 2, 2, 1, 0].forEach((index, at) => view.setUint16(12 + padded + at * 2, index, true));
  const created = decodeCreate(bytes);
  assert.equal(created.handle, 7);
  assert.equal(created.description.drawables[1]!.masks[0], 0);
  assert.deepEqual([...created.description.indices[1]!], [2, 1, 0]);
  // The same bytes at an unaligned offset decode the same.
  const shifted = new Uint8Array(bytes.length + 1);
  shifted.set(bytes, 1);
  assert.deepEqual([...decodeCreate(shifted.subarray(1)).description.indices[0]!], [0, 1, 2]);
});

test("encodeUpdate lays the request out as cubism_bridge.h describes", () => {
  const bytes = encodeUpdate({handle: 9, forceAll: true, transform: [1, -2, 3, 4], parameters: [0.5, 0.25], partOpacities: [1]});
  const view = new DataView(bytes.buffer);
  assert.equal(bytes.length, 4 + 4 + 16 + 4 + 8 + 4 + 4);
  assert.deepEqual([view.getUint32(0, true), view.getUint32(4, true)], [9, 1]);
  assert.deepEqual([8, 12, 16, 20].map((at) => view.getFloat32(at, true)), [1, -2, 3, 4]);
  assert.equal(view.getUint32(24, true), 2);
  assert.deepEqual([28, 32].map((at) => view.getFloat32(at, true)), [0.5, 0.25]);
  assert.equal(view.getUint32(36, true), 1);
});

test("decodeUpdate returns vertices only for drawables whose flags say they changed", () => {
  const model = description([{}, {}, {}]);
  const flags = [DYNAMIC_VISIBLE | DYNAMIC_POSITIONS_CHANGED, DYNAMIC_VISIBLE, DYNAMIC_VISIBLE | DYNAMIC_POSITIONS_CHANGED];
  const floats = 4 * VERTEX_FLOATS;
  const head = 8 + 4; // status, count, 3 flag bytes padded to 4
  const bytes = new Uint8Array(head + 3 * 4 + 2 * floats * 4);
  const view = new DataView(bytes.buffer);
  view.setUint32(4, 3, true);
  flags.forEach((flag, at) => { bytes[8 + at] = flag; });
  [2, 0, 1].forEach((order, at) => view.setInt32(head + at * 4, order, true));
  for (let at = 0; at < 2 * floats; at += 1) view.setFloat32(head + 12 + at * 4, at, true);
  const frame = decodeUpdate(bytes, model);
  assert.deepEqual([...frame.renderOrders], [2, 0, 1]);
  assert.equal(frame.vertices[1], null);
  assert.equal(frame.vertices[0]!.length, floats);
  assert.equal(frame.vertices[2]![0], floats);
  assert.throws(() => decodeUpdate(bytes.subarray(0, bytes.length - 4), model), /unread bytes|offset is outside|out of range|length/i);
  assert.throws(() => decodeUpdate(bytes, description([{}, {}])), /covers 3 drawables, the model has 2/);
});

// ---- renderer ------------------------------------------------------------

function frameOf(model: ModelDescription, options: {visible?: boolean[]; orders?: number[]; alpha?: number[]; changed?: boolean[]} = {}): UpdateFrame {
  const count = model.drawables.length;
  const flags = new Uint8Array(count);
  const vertices = model.drawables.map((drawable, at) => {
    const changed = options.changed?.[at] ?? true;
    flags[at] = ((options.visible?.[at] ?? true) ? DYNAMIC_VISIBLE : 0) | (changed ? DYNAMIC_POSITIONS_CHANGED : 0);
    if (!changed) return null;
    const data = new Float32Array(drawable.vc * VERTEX_FLOATS);
    for (let v = 0; v < drawable.vc; v += 1) data.set([at * 100 + v, at, 0, 0, 1, 1, 1, options.alpha?.[at] ?? 1], v * VERTEX_FLOATS);
    return data;
  });
  return {flags, renderOrders: Int32Array.from(options.orders ?? model.drawables.map((_, at) => at)), vertices};
}

function rendererFor(model: ModelDescription, budget = {remaining: 60}) {
  const raw = new FakeRaw();
  const warnings: string[] = [];
  const renderer = new ModelRenderer({
    name: "m", raw, description: model, textures: [0, 1, 2].map((index) => raw.asset(`t${index}.png`)),
    phase: "after-stage", order: 5000, budget, warn: (message) => warnings.push(message),
  });
  return {raw, renderer, warnings, budget};
}

test("drawables that share texture, blend and mask are drawn by one mesh, in render order", () => {
  const model = description([{texture: 0}, {texture: 0}, {texture: 1}, {texture: 0, flags: BLEND_ADDITIVE}, {texture: 0, flags: BLEND_MULTIPLICATIVE}]);
  const {raw, renderer} = rendererFor(model);
  raw.beginFrame();
  renderer.apply(frameOf(model));
  const drawn = raw.drawn();
  assert.deepEqual(drawn.map((mesh) => [mesh.options.blend, (mesh.options.texture as {path: string}).path, mesh.vertexCount]),
    [["normal", "t0.png", 8], ["normal", "t1.png", 4], ["additive", "t0.png", 4], ["multiply", "t0.png", 4]]);
  assert.deepEqual(drawn.map((mesh) => mesh.options.order), [5000, 5001, 5002, 5003]);
  // The second drawable's indices were shifted past the first's four vertices.
  assert.deepEqual([...drawn[0]!.indexData], [0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7]);
  // x of the first vertex of each member shows which drawable went where.
  assert.deepEqual([drawn[0]!.vertexData[0], drawn[0]!.vertexData[4 * VERTEX_FLOATS]], [0, 100]);
});

test("a change of render order, visibility or opacity re-lays out only when it must", () => {
  const model = description([{texture: 0}, {texture: 1}, {texture: 0}]);
  const {raw, renderer} = rendererFor(model);
  raw.beginFrame();
  renderer.apply(frameOf(model));
  assert.equal(raw.drawn().length, 3);

  raw.beginFrame();
  renderer.apply(frameOf(model, {changed: [true, false, false]}));
  assert.equal(raw.uploadsThisFrame, 1, "moving one drawable is one vertex upload");
  assert.equal(raw.drawn().length, 3);

  raw.beginFrame();
  renderer.apply(frameOf(model, {orders: [0, 1, 0], changed: [false, false, false]}));
  assert.deepEqual(raw.drawn().map((mesh) => mesh.vertexCount), [8, 4], "the two texture-0 drawables are adjacent now");

  raw.beginFrame();
  renderer.apply(frameOf(model, {visible: [true, false, true], changed: [false, false, false]}));
  assert.deepEqual(raw.drawn().map((mesh) => mesh.vertexCount), [8]);

  raw.beginFrame();
  renderer.apply(frameOf(model, {visible: [true, false, true], alpha: [1, 1, 0], changed: [false, false, true]}));
  assert.deepEqual(raw.drawn().map((mesh) => mesh.vertexCount), [4], "a fully transparent drawable is not drawn");
});

test("a set of masks becomes one hidden union mesh, opaque, that the masked run points at", () => {
  const model = description([{texture: 0, masks: [1, 2]}, {texture: 0, flags: 0}, {texture: 0}, {texture: 0, masks: [2, 1]}], 4);
  const {raw, renderer} = rendererFor(model);
  raw.beginFrame();
  renderer.apply(frameOf(model, {alpha: [1, 0.3, 0.3, 1]}));
  const masked = raw.drawn().filter((mesh) => mesh.options.mask);
  assert.equal(masked.length, 2, "drawables 0 and 3 have the same mask set (in either order) but drawables 1 and 2 are drawn between them");
  const maskIds = new Set(raw.drawn().map((mesh) => mesh.options.mask).filter(Boolean));
  assert.equal(maskIds.size, 1, "one union mesh serves both runs");
  const union = raw.meshes.get([...maskIds][0]!)!;
  assert.equal(union.options.visible, false);
  assert.equal(union.vertexCount, 8, "drawables 1 and 2");
  for (let v = 0; v < 8; v += 1) assert.equal(union.vertexData[v * VERTEX_FLOATS + 7], 1, "coverage ignores the mask drawable's own opacity");
});

test("masks on different texture pages keep one page and warn; inverted masks draw unmasked and warn", () => {
  const model = description([{texture: 0, masks: [1, 2]}, {texture: 0}, {texture: 1}, {texture: 0, flags: INVERTED_MASK, masks: [1]}]);
  const {raw, renderer, warnings} = rendererFor(model);
  raw.beginFrame();
  renderer.apply(frameOf(model));
  assert.equal(warnings.length, 2);
  assert.ok(warnings.some((message) => /more than one texture page/.test(message)));
  assert.ok(warnings.some((message) => /inverted mask/.test(message)));
  const union = raw.meshes.get(raw.drawn().find((mesh) => mesh.options.mask)!.options.mask!)!;
  assert.equal(union.vertexCount, 4);
});

test("indices are never uploaded before their vertices, and a re-layout into a smaller mesh is fine", () => {
  const model = description([{texture: 0}, {texture: 1}, {texture: 0}]);
  const {raw, renderer} = rendererFor(model);
  raw.beginFrame();
  renderer.apply(frameOf(model));
  raw.beginFrame();
  // Merging drawables 0 and 2 grows run 0 while run 1 shrinks to nothing and run 2 disappears.
  renderer.apply(frameOf(model, {orders: [0, 2, 1], changed: [false, false, false]}));
  assert.deepEqual(raw.drawn().map((mesh) => mesh.vertexCount), [8, 4]);
});

test("a heavy layout is spread over frames and never exceeds the upload budget", () => {
  // 100 drawables alternating blend modes: 100 runs, 200 uploads to set up.
  const model = description(Array.from({length: 100}, (_, at) => ({texture: 0, flags: at % 2 ? BLEND_ADDITIVE : 0})));
  const {raw, renderer, budget} = rendererFor(model);
  let frames = 0;
  do {
    raw.beginFrame();
    budget.remaining = 60;
    renderer.apply(frameOf(model, {changed: model.drawables.map(() => frames === 0)}));
    frames += 1;
    assert.ok(raw.uploadsThisFrame <= 60, `frame ${frames} used ${raw.uploadsThisFrame} uploads`);
  } while (renderer.pending && frames < 20);
  assert.equal(renderer.pending, false);
  assert.ok(frames >= 4 && frames <= 6, `settled in ${frames} frames`);
  assert.equal(raw.drawn().length, 100);
  assert.ok(raw.maxUploadsInAFrame <= MAX_UPLOADS_PER_FRAME);
});

test("runs are capped so their indices fit 16 bits", () => {
  const big = MAX_RUN_VERTICES / 2 + 1;
  const model = description([{vc: big, ic: 3}, {vc: big, ic: 3}], 0);
  const {raw, renderer} = rendererFor(model);
  raw.beginFrame();
  renderer.apply(frameOf(model));
  assert.equal(raw.drawn().length, 2);
});

test("dispose destroys every mesh", () => {
  const model = description([{texture: 0, masks: [1]}, {texture: 0}]);
  const {raw, renderer} = rendererFor(model);
  raw.beginFrame();
  renderer.apply(frameOf(model));
  assert.ok(raw.meshes.size >= 2);
  renderer.dispose();
  assert.equal(raw.meshes.size, 0);
});
