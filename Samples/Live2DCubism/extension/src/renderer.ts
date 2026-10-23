// Turns the drawables Core reports into raw meshes.
//
// A model has 100-300 drawables, but a raw extension may upload only 64 times
// a frame (vertices and indices both count), so drawables that are drawn one
// after another with the same texture, blend mode and mask are merged into one
// mesh (a "run"). Runs are rebuilt only when what is visible or its order
// changes; on an ordinary frame a run costs one upload.
//
// The frame budget is shared by every model of the extension (`UploadBudget`).
// Work that does not fit is finished on the next frames, so a model that
// appears or reorders may be drawn from a mix of old and new runs for a frame
// or two instead of failing with PXJS7801.
//
// Masks: a drawable may be clipped by several mask drawables (their union).
// The raw API masks a mesh with one other mesh, so each distinct set of masks
// becomes one hidden "union" mesh that holds all of the set's geometry. Masks
// of one set must share a texture page; later pages are dropped with a warning.
// Inverted masks are not available in the raw API and are drawn unmasked.

import {
  BLEND_ADDITIVE,
  BLEND_MULTIPLICATIVE,
  DYNAMIC_VISIBLE,
  INVERTED_MASK,
  VERTEX_FLOATS,
  type ModelDescription,
  type UpdateFrame,
} from "./protocol.ts";

// The slice of @prismatix/runtime/raw this file uses, so the renderer runs
// against a fake in tests and the real thing (structurally the same) in the Player.
export type RenderPhaseLike = "before-stage" | "after-stage" | "before-ui" | "after-ui";
export type BlendLike = "normal" | "additive" | "multiply";
export type TextureSourceLike =
  | {readonly kind: "asset"; readonly path: string}
  | {readonly kind: "texture"; readonly id: string}
  | {readonly kind: "target"; readonly id: string};

export interface MeshOptionsLike {
  readonly phase?: RenderPhaseLike;
  readonly order?: number;
  readonly visible?: boolean;
  readonly route?: string;
  readonly texture?: TextureSourceLike | null;
  readonly blend?: BlendLike;
  readonly mask?: string | null;
}

export interface MeshLike {
  readonly id: string;
  update(options: MeshOptionsLike): boolean;
  vertices(data: Float32Array, firstVertex?: number): void;
  indices(data: Uint16Array | Uint32Array): void;
  destroy(): boolean;
}

export interface RawLike {
  createMesh(id: string, options?: MeshOptionsLike): MeshLike;
  /** A packaged image as a mesh texture. */
  asset(path: string): TextureSourceLike;
}

/** Uploads left this frame, shared by every model; the scene refills it each frame. */
export interface UploadBudget {
  remaining: number;
}

export interface RendererOptions {
  /** Prefix of every mesh id this model creates; unique per model instance. */
  readonly name: string;
  readonly raw: RawLike;
  readonly description: ModelDescription;
  /** Texture sources by Cubism texture index (`raw.asset(path)`). */
  readonly textures: readonly TextureSourceLike[];
  readonly phase: RenderPhaseLike;
  /** Meshes of this model draw in [order, order + 1000). */
  readonly order: number;
  readonly route?: string;
  readonly budget: UploadBudget;
  readonly warn: (message: string) => void;
}

/** A mesh's vertex limit is 65,536; stay under it so indices fit 16 bits. */
export const MAX_RUN_VERTICES = 60000;

interface Piece {
  mesh: MeshLike;
  members: number[];
  offsets: number[];
  buffer: Float32Array;
  indices: Uint16Array;
  /** The mesh needs its vertices, indices and options (a new layout). */
  setup: boolean;
  /** A member has new vertices. */
  stale: boolean;
}

interface Run extends Piece {
  signature: string;
  options: MeshOptionsLike;
}

interface MaskSet extends Piece {
  used: boolean;
}

function concatenate(drawables: ModelDescription["drawables"], indices: ModelDescription["indices"], members: readonly number[]) {
  const offsets: number[] = [];
  let vertexTotal = 0, indexTotal = 0;
  for (const at of members) {
    offsets.push(vertexTotal * VERTEX_FLOATS);
    vertexTotal += drawables[at]!.vc;
    indexTotal += drawables[at]!.ic;
  }
  const combined = new Uint16Array(indexTotal);
  let written = 0, base = 0;
  for (const at of members) {
    const source = indices[at]!;
    for (let k = 0; k < source.length; k += 1) combined[written++] = source[k]! + base;
    base += drawables[at]!.vc;
  }
  return {offsets, buffer: new Float32Array(vertexTotal * VERTEX_FLOATS), indices: combined};
}

export class ModelRenderer {
  private readonly options: RendererOptions;
  private readonly drawables: ModelDescription["drawables"];
  private readonly vertexData: Float32Array[];
  private readonly shown: boolean[];
  private readonly renderOrders: Int32Array;
  private readonly changed = new Set<number>();
  private readonly runs: Run[] = [];
  private readonly maskSets = new Map<string, MaskSet>();
  /** Slot of the run each shown drawable is in, and the mask sets it belongs to. */
  private ownerRun = new Map<number, number>();
  private ownerMasks = new Map<number, MaskSet[]>();
  private structureDirty = true;
  private maskSerial = 0;
  private warnedInverted = false;
  /** Uploads this renderer made in the last `apply` (diagnostics, tests). */
  uploads = 0;

  constructor(options: RendererOptions) {
    this.options = options;
    this.drawables = options.description.drawables;
    this.vertexData = this.drawables.map((drawable) => new Float32Array(drawable.vc * VERTEX_FLOATS));
    this.shown = this.drawables.map(() => false);
    this.renderOrders = new Int32Array(this.drawables.length).fill(-1);
  }

  /** True while layout or upload work is waiting for budget. */
  get pending(): boolean {
    return this.structureDirty || this.runs.some((run) => run.setup || run.stale) ||
      [...this.maskSets.values()].some((set) => set.setup || set.stale);
  }

  /** Takes one update from Core, rebuilds what changed and uploads what the budget allows. */
  apply(frame: UpdateFrame): void {
    this.uploads = 0;
    this.changed.clear();
    for (let at = 0; at < this.drawables.length; at += 1) {
      const incoming = frame.vertices[at];
      if (incoming) {
        this.vertexData[at]!.set(incoming);
        this.changed.add(at);
      }
      const drawable = this.drawables[at]!;
      const alpha = drawable.vc > 0 ? this.vertexData[at]![7]! : 0;
      const shown = (frame.flags[at]! & DYNAMIC_VISIBLE) !== 0 && alpha > 0;
      if (shown !== this.shown[at] || frame.renderOrders[at] !== this.renderOrders[at]) this.structureDirty = true;
      this.shown[at] = shown;
      this.renderOrders[at] = frame.renderOrders[at]!;
    }
    if (this.structureDirty) this.layout();
    for (const at of this.changed) {
      const slot = this.ownerRun.get(at);
      if (slot !== undefined && !this.runs[slot]!.setup) this.runs[slot]!.stale = true;
      for (const set of this.ownerMasks.get(at) ?? []) if (!set.setup) set.stale = true;
    }
    this.flush();
  }

  dispose(): void {
    for (const run of this.runs) run.mesh.destroy();
    this.runs.length = 0;
    for (const set of this.maskSets.values()) set.mesh.destroy();
    this.maskSets.clear();
    this.ownerRun.clear();
    this.ownerMasks.clear();
  }

  private blendOf(flags: number): BlendLike {
    return (flags & BLEND_ADDITIVE) !== 0 ? "additive" : (flags & BLEND_MULTIPLICATIVE) !== 0 ? "multiply" : "normal";
  }

  private maskKeyOf(at: number): string {
    const drawable = this.drawables[at]!;
    if (drawable.masks.length === 0) return "";
    if ((drawable.flags & INVERTED_MASK) !== 0) {
      if (!this.warnedInverted) {
        this.warnedInverted = true;
        this.options.warn(`${this.options.name}: ${drawable.id} uses an inverted mask, which raw meshes cannot do; it is drawn unmasked`);
      }
      return "";
    }
    return [...drawable.masks].sort((a, b) => a - b).join(",");
  }

  /** Decides which drawables share which mesh; touches the raw API only to create meshes. */
  private layout(): void {
    this.structureDirty = false;
    const order: number[] = [];
    for (let at = 0; at < this.drawables.length; at += 1) if (this.shown[at]) order.push(at);
    order.sort((a, b) => this.renderOrders[a]! - this.renderOrders[b]! || a - b);

    const plan: {key: string; maskKey: string; members: number[]; vertices: number}[] = [];
    for (const at of order) {
      const drawable = this.drawables[at]!;
      const maskKey = this.maskKeyOf(at);
      const key = `${drawable.texture}|${this.blendOf(drawable.flags)}|${maskKey}`;
      const last = plan[plan.length - 1];
      if (last && last.key === key && last.vertices + drawable.vc <= MAX_RUN_VERTICES) {
        last.members.push(at);
        last.vertices += drawable.vc;
      } else {
        plan.push({key, maskKey, members: [at], vertices: drawable.vc});
      }
    }

    for (const set of this.maskSets.values()) set.used = false;
    this.ownerRun = new Map();
    plan.forEach((entry, slot) => {
      for (const at of entry.members) this.ownerRun.set(at, slot);
      const maskId = entry.maskKey === "" ? null : this.ensureMaskSet(entry.maskKey);
      const signature = `${entry.key}:${entry.members.join(",")}`;
      const existing = this.runs[slot];
      if (existing && existing.signature === signature) return;
      const first = this.drawables[entry.members[0]!]!;
      const built = concatenate(this.drawables, this.options.description.indices, entry.members);
      const mesh = existing?.mesh ?? this.options.raw.createMesh(`${this.options.name}.run${slot}`, {
        phase: this.options.phase, visible: false,
        ...(this.options.route === undefined ? {} : {route: this.options.route}),
      });
      this.runs[slot] = {
        mesh, signature, members: entry.members, ...built, setup: true, stale: false,
        options: {
          visible: true, order: this.options.order + slot,
          texture: this.options.textures[first.texture] ?? null, blend: this.blendOf(first.flags), mask: maskId,
        },
      };
    });
    for (let slot = plan.length; slot < this.runs.length; slot += 1) {
      const stale = this.runs[slot]!;
      stale.mesh.update({visible: false});
      stale.signature = "";
      stale.members = [];
      stale.setup = stale.stale = false;
    }

    this.ownerMasks = new Map();
    for (const [key, set] of this.maskSets) {
      if (!set.used) {
        set.mesh.destroy();
        this.maskSets.delete(key);
        continue;
      }
      for (const at of set.members) this.ownerMasks.set(at, [...(this.ownerMasks.get(at) ?? []), set]);
    }
  }

  /** The id of the hidden union mesh for a set of mask drawables, creating it when new. */
  private ensureMaskSet(key: string): string {
    let set = this.maskSets.get(key);
    if (!set) {
      const wanted = key.split(",").map(Number);
      const texture = this.drawables[wanted[0]!]!.texture;
      const members = wanted.filter((at) => this.drawables[at]!.texture === texture);
      if (members.length < wanted.length)
        this.options.warn(`${this.options.name}: masks of one drawable use more than one texture page; only page ${texture} clips`);
      const built = concatenate(this.drawables, this.options.description.indices, members);
      const mesh = this.options.raw.createMesh(`${this.options.name}.mask${this.maskSerial++}`, {
        phase: this.options.phase, visible: false, texture: this.options.textures[texture] ?? null, blend: "normal",
        ...(this.options.route === undefined ? {} : {route: this.options.route}),
      });
      set = {mesh, members, ...built, setup: true, stale: false, used: true};
      this.maskSets.set(key, set);
    }
    set.used = true;
    return set.mesh.id;
  }

  /** Copies every member: an update deferred by the budget may span several frames of changes. */
  private fill(piece: Piece, forceOpaque: boolean): void {
    piece.members.forEach((at, slot) => {
      const start = piece.offsets[slot]!;
      piece.buffer.set(this.vertexData[at]!, start);
      // Coverage of a mask is its texture alone, whatever the drawable's own opacity.
      if (forceOpaque)
        for (let k = start + 7, end = start + this.drawables[at]!.vc * VERTEX_FLOATS; k < end; k += VERTEX_FLOATS) piece.buffer[k] = 1;
    });
  }

  private spend(cost: number): boolean {
    if (this.options.budget.remaining < cost) return false;
    this.options.budget.remaining -= cost;
    this.uploads += cost;
    return true;
  }

  /** Vertices go first: the Player rejects indices past the mesh's vertices. */
  private flush(): void {
    for (const set of this.maskSets.values()) {
      if (set.setup) {
        if (!this.spend(2)) continue;
        this.fill(set, true);
        set.mesh.vertices(set.buffer);
        set.mesh.indices(set.indices);
        set.setup = set.stale = false;
      } else if (set.stale && this.spend(1)) {
        this.fill(set, true);
        set.mesh.vertices(set.buffer);
        set.stale = false;
      }
    }
    for (const run of this.runs) {
      if (run.members.length === 0) continue;
      if (run.setup) {
        if (!this.spend(2)) continue;
        this.fill(run, false);
        run.mesh.vertices(run.buffer);
        run.mesh.indices(run.indices);
        run.mesh.update(run.options);
        run.setup = run.stale = false;
      } else if (run.stale && this.spend(1)) {
        this.fill(run, false);
        run.mesh.vertices(run.buffer);
        run.stale = false;
      }
    }
  }
}
