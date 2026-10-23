// Live2D (Cubism) for PrismatiX: Story commands over the native Core bridge
// and raw meshes. docs of the whole arrangement: ../README.md

import {createPrismatiXContext, defineCommand} from "@prismatix/runtime";
import {createPrismatiXNative} from "@prismatix/runtime/native";
import {createPrismatiXRaw} from "@prismatix/runtime/raw";

import {Live2DScene, type ModelSpec, type SceneState} from "./src/scene.ts";

const ctx = createPrismatiXContext();
const raw = createPrismatiXRaw();
const native = createPrismatiXNative();

// Saved with the game and restored by load and rollback. Meshes, Core handles
// and motion clocks are not: the frame loop below rebuilds them from this.
const scene = ctx.state.define<SceneState>("live2d.scene", {version: 1, initial: {core: "", models: {}}});

/** Replaces one model's spec; commands for a model that is not shown are ignored with a warning. */
function edit(id: string, change: (spec: ModelSpec) => ModelSpec): void {
  const current = scene.get().models[id];
  if (!current) {
    ctx.log.warn(`Live2D: no model '${id}' is shown; use live2d.show first`);
    return;
  }
  scene.update((state) => ({...state, models: {...state.models, [id]: change(current)}}));
}

defineCommand("live2d.core", ({path}) => {
  scene.update((state) => ({...state, core: path}));
});

defineCommand("live2d.show", ({id, model, x, y, height, z, idle, breath, blink}) => {
  const spec: ModelSpec = {
    model, x, y, height, z, idle, breath, blink, motion: null, expression: "", params: {},
  };
  scene.update((state) => ({...state, models: {...state.models, [id]: spec}}));
});

defineCommand("live2d.hide", ({id}) => {
  scene.update((state) => {
    const {[id]: _removed, ...rest} = state.models;
    return {...state, models: rest};
  });
});

defineCommand("live2d.move", ({id, x, y, height}) => {
  edit(id, (spec) => ({...spec, x, y, height}));
});

defineCommand("live2d.motion", ({id, group, index, loop}) => {
  edit(id, (spec) => ({...spec, motion: {group, index, loop, serial: (spec.motion?.serial ?? 0) + 1}}));
});

defineCommand("live2d.expression", ({id, name}) => {
  edit(id, (spec) => ({...spec, expression: name}));
});

// Holds a parameter at a value (ParamMouthOpenY for lip sync, ParamAngleX to look
// somewhere); `release` lets go of it.
defineCommand("live2d.param", ({id, param, value, release}) => {
  edit(id, (spec) => {
    const {[param]: _removed, ...rest} = spec.params;
    return {...spec, params: release ? rest : {...rest, [param]: value}};
  });
});

const live2d = new Live2DScene({
  readText: (path) => ctx.assets.readText(path),
  native: (service, payload) => native.request(service, payload),
  raw,
  logicalSize: () => ctx.renderer.logicalSize(),
  log: {info: (m) => ctx.log.info(m), warn: (m) => ctx.log.warn(m), error: (m) => ctx.log.error(m)},
  // The Player allows 64 uploads a frame; keep a few for anything else.
  uploadsPerFrame: 60,
  phase: "after-stage",
});

ctx.frame.onUpdate((deltaSeconds) => {
  live2d.update(deltaSeconds, scene.get());
});

ctx.events.on("engine.shutdown", () => {
  live2d.dispose();
});
