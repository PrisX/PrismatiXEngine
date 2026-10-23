// The models on screen. `SceneState` is what a save holds (which models, where,
// what they were told to do); everything else (Core handles, meshes, the clock
// of a motion) is rebuilt from it by `Live2DScene.update`, which runs every
// frame. Commands only edit the state, so a load or rollback needs no special
// handling: the next frame makes the screen match whatever state came back.

import {decodeBase64} from "./base64.ts";
import {parseModel3, type Model3} from "./model3.ts";
import {
  applyBreath, Blinker, ExpressionPlayer, MotionPlayer, parseExpression, parseMotion,
  type Expression, type Motion,
} from "./motion.ts";
import {ParameterBank} from "./parameters.ts";
import {
  decodeCoreLoaded, decodeCreate, decodeUpdate, encodeUpdate, encodeUtf8, type CreatedModel,
} from "./protocol.ts";
import {ModelRenderer, type RawLike, type RenderPhaseLike, type UploadBudget} from "./renderer.ts";

export type MotionRequest = {
  readonly group: string;
  readonly index: number;
  readonly loop: boolean;
  /** Changes with every command, so asking for the same motion again restarts it. */
  readonly serial: number;
};

export type ModelSpec = {
  /** Project path of the .model3.json. */
  readonly model: string;
  /** Where the bottom centre of the canvas stands, as fractions of the screen (0.5, 1 is bottom centre). */
  readonly x: number;
  readonly y: number;
  /** How tall the canvas is, as a fraction of the screen height. */
  readonly height: number;
  readonly z: number;
  /** Motion group looped while nothing else plays ("" for none). */
  readonly idle: string;
  readonly breath: boolean;
  readonly blink: boolean;
  readonly motion: MotionRequest | null;
  /** Expression name from the model3.json ("" for none). */
  readonly expression: string;
  /** Parameters the script holds at a value (lip sync, head turns, ...). */
  readonly params: {readonly [id: string]: number};
};

export type SceneState = {
  /** Path of the Cubism Core library; empty uses the platform's default name. */
  readonly core: string;
  readonly models: {readonly [id: string]: ModelSpec};
};

export interface SceneHost {
  readText(path: string): string;
  /** Calls a native service synchronously. */
  native(service: string, payload: Uint8Array): Uint8Array;
  readonly raw: RawLike;
  /** The logical screen size the fractions in `ModelSpec` refer to. */
  logicalSize(): {readonly w: number; readonly h: number};
  readonly log: {info(message: string): void; warn(message: string): void; error(message: string): void};
  /** Uploads one frame may spend across all models (the Player allows 64). */
  readonly uploadsPerFrame: number;
  readonly phase: RenderPhaseLike;
}

function hash(text: string): number {
  let value = 2166136261;
  for (let index = 0; index < text.length; index += 1) value = Math.imul(value ^ text.charCodeAt(index), 16777619);
  return value >>> 0;
}

/** Mesh ids are letters, digits, dot, dash and underscore. */
function meshPrefix(id: string): string {
  return `l2d.${id.replace(/[^A-Za-z0-9._-]/gu, "_")}`;
}

class ModelInstance {
  readonly modelPath: string;
  private readonly host: SceneHost;
  private readonly model3: Model3;
  private readonly created: CreatedModel;
  private readonly renderer: ModelRenderer;
  private readonly bank: ParameterBank;
  private readonly blinker: Blinker;
  private readonly motions = new Map<string, Motion>();
  private readonly expressions = new Map<string, Expression>();
  private motion: MotionPlayer | null = null;
  private motionSerial = -1;
  private idlePlaying = "";
  private expression: ExpressionPlayer | null = null;
  private expressionName = "";
  private clock = 0;
  private lastTransform = "";
  private readonly warned = new Set<string>();

  constructor(id: string, spec: ModelSpec, host: SceneHost, budget: UploadBudget) {
    this.host = host;
    this.modelPath = spec.model;
    this.model3 = parseModel3(host.readText(spec.model), spec.model);
    const moc = decodeBase64(host.readText(`${this.model3.moc}.b64`));
    this.created = decodeCreate(host.native("live2d.model.create", moc));
    const description = this.created.description;
    this.bank = new ParameterBank(description.parameters, description.parts);
    this.blinker = new Blinker(this.model3.eyeBlinkIds.filter((eye) => this.bank.has(eye)), hash(id));
    this.renderer = new ModelRenderer({
      name: meshPrefix(id), raw: host.raw, description,
      textures: this.model3.textures.map((path) => host.raw.asset(path)),
      phase: host.phase, order: spec.z * 1000, budget,
      warn: (message) => host.log.warn(message),
    });
  }

  private warnOnce(message: string): void {
    if (this.warned.has(message)) return;
    this.warned.add(message);
    this.host.log.warn(message);
  }

  private loadMotion(group: string, index: number): {motion: Motion; fadeIn: number | undefined; fadeOut: number | undefined} | null {
    const entry = this.model3.motions[group]?.[index];
    if (!entry) {
      this.warnOnce(`Live2D: ${this.modelPath} has no motion ${group}[${index}]`);
      return null;
    }
    let motion = this.motions.get(entry.file);
    if (!motion) {
      motion = parseMotion(this.host.readText(entry.file));
      this.motions.set(entry.file, motion);
    }
    return {motion, fadeIn: entry.fadeIn, fadeOut: entry.fadeOut};
  }

  private play(group: string, index: number, loop: boolean): boolean {
    const loaded = this.loadMotion(group, index);
    if (!loaded) return false;
    this.motion = new MotionPlayer(loaded.motion, loop || loaded.motion.loop, loaded.fadeIn, loaded.fadeOut);
    return true;
  }

  private chooseMotion(spec: ModelSpec): void {
    if (spec.motion && spec.motion.serial !== this.motionSerial) {
      this.motionSerial = spec.motion.serial;
      this.idlePlaying = "";
      this.play(spec.motion.group, spec.motion.index, spec.motion.loop);
    }
    if (this.motion?.finished) this.motion = null;
    if (!this.motion && spec.idle !== "") {
      this.idlePlaying = spec.idle;
      this.play(spec.idle, 0, true);
    } else if (this.motion && this.idlePlaying !== "" && spec.idle !== this.idlePlaying) {
      this.motion = null;
      this.idlePlaying = "";
    }
  }

  private chooseExpression(spec: ModelSpec): void {
    if (spec.expression === this.expressionName) return;
    this.expressionName = spec.expression;
    this.expression = null;
    if (spec.expression === "") return;
    const path = this.model3.expressions[spec.expression];
    if (!path) {
      this.warnOnce(`Live2D: ${this.modelPath} has no expression '${spec.expression}'`);
      return;
    }
    let expression = this.expressions.get(path);
    if (!expression) {
      expression = parseExpression(this.host.readText(path));
      this.expressions.set(path, expression);
    }
    this.expression = new ExpressionPlayer(expression);
  }

  tick(deltaSeconds: number, spec: ModelSpec): void {
    this.clock += deltaSeconds;
    this.chooseMotion(spec);
    this.chooseExpression(spec);
    this.bank.reset();
    if (this.motion) {
      this.motion.advance(deltaSeconds);
      this.motion.apply(this.bank, 1);
    }
    if (this.expression) {
      this.expression.advance(deltaSeconds);
      this.expression.apply(this.bank, 1);
    }
    if (spec.breath) applyBreath(this.bank, this.clock);
    if (spec.blink) {
      this.blinker.advance(deltaSeconds);
      this.blinker.apply(this.bank, (parameter) => this.motion?.controls(parameter) ?? false);
    }
    for (const [parameter, value] of Object.entries(spec.params)) this.bank.set(parameter, value);

    // Model units to screen: the canvas is spec.height of the screen tall and
    // stands on (spec.x, spec.y). Model space has y up and its origin at
    // (ox, oy) of the canvas, measured from the canvas's top-left.
    const canvas = this.created.description.canvas;
    const screen = this.host.logicalSize();
    const scale = (spec.height * screen.h) / canvas.h;
    const transform = [canvas.ppu * scale, -canvas.ppu * scale,
      spec.x * screen.w + (canvas.ox - canvas.w / 2) * scale, spec.y * screen.h - (canvas.h - canvas.oy) * scale] as const;
    const signature = transform.join(",");
    const request = encodeUpdate({
      handle: this.created.handle, forceAll: signature !== this.lastTransform, transform,
      parameters: this.bank.values, partOpacities: this.bank.partValues,
    });
    this.lastTransform = signature;
    this.renderer.apply(decodeUpdate(this.host.native("live2d.model.update", request), this.created.description));
  }

  dispose(): void {
    this.renderer.dispose();
    const handle = new Uint8Array(4);
    new DataView(handle.buffer).setUint32(0, this.created.handle, true);
    try {
      this.host.native("live2d.model.release", handle);
    } catch (error) {
      this.host.log.warn(`Live2D: releasing a model failed: ${String(error)}`);
    }
  }
}

export class Live2DScene {
  private readonly host: SceneHost;
  private readonly instances = new Map<string, ModelInstance>();
  /** (id|model) pairs that failed to load, so a bad model is reported once, not every frame. */
  private readonly failed = new Set<string>();
  private readonly budget: UploadBudget = {remaining: 0};
  private coreLoaded: string | null = null;
  private coreFailed: string | null = null;

  constructor(host: SceneHost) {
    this.host = host;
  }

  /** Models currently on screen (tests, diagnostics). */
  get loaded(): readonly string[] {
    return [...this.instances.keys()];
  }

  private ensureCore(path: string): boolean {
    if (this.coreLoaded === path) return true;
    if (this.coreFailed === path) return false;
    try {
      const version = decodeCoreLoaded(this.host.native("live2d.core.load", encodeUtf8(path)));
      this.host.log.info(`Live2D: Cubism Core 0x${version.toString(16)} loaded`);
      this.coreLoaded = path;
      return true;
    } catch (error) {
      this.coreFailed = path;
      this.host.log.error(`Live2D: ${String(error)}`);
      return false;
    }
  }

  update(deltaSeconds: number, state: SceneState): void {
    this.budget.remaining = this.host.uploadsPerFrame;
    for (const [id, instance] of this.instances) {
      if (state.models[id]?.model === instance.modelPath) continue;
      instance.dispose();
      this.instances.delete(id);
    }
    const wanted = Object.entries(state.models);
    if (wanted.length === 0) return;
    if (!this.ensureCore(state.core)) return;
    for (const [id, spec] of wanted) {
      let instance = this.instances.get(id);
      const failure = `${id}|${spec.model}`;
      if (!instance) {
        if (this.failed.has(failure)) continue;
        try {
          instance = new ModelInstance(id, spec, this.host, this.budget);
          this.instances.set(id, instance);
        } catch (error) {
          this.failed.add(failure);
          this.host.log.error(`Live2D: cannot load '${id}' (${spec.model}): ${String(error)}`);
          continue;
        }
      }
      try {
        instance.tick(deltaSeconds, spec);
      } catch (error) {
        this.failed.add(failure);
        instance.dispose();
        this.instances.delete(id);
        this.host.log.error(`Live2D: '${id}' stopped: ${String(error)}`);
      }
    }
  }

  dispose(): void {
    for (const instance of this.instances.values()) instance.dispose();
    this.instances.clear();
    this.coreLoaded = null;
  }
}
