// @prismatix/runtime/raw: low-level drawing for extensions (layer 1).
//
// EXPERIMENTAL. The shape of this module may change in a minor release.
//
// `createPrismatiXContext()` stays the high-level facade. This entry is a
// separate surface over the Player's `EngineRaw` host object: meshes built
// from vertex and index arrays, textures the script writes pixels into, and
// render targets meshes can draw into. It works the same in the desktop
// Player, the WebGL2 and WebGPU Web Players and the WASM Preview.
//
// Rules the Player enforces:
//
// - Each call needs a manifest capability: "raw.mesh" for meshes,
//   "raw.texture" for writable textures, "raw.target" for render targets.
//   A call without it throws PermissionError PXJS7101; builds warn
//   (PXCLIEXT2004).
// - Raw resources are presentation only: they are not saved, not journaled
//   and not rolled back. A load or rollback keeps them as they are, so
//   rebuild them from saved state (`ctx.state`) when that matters, e.g. in a
//   `save.loaded` listener. `create*` on an existing id replaces it, so a
//   replayed handler that creates its meshes again is harmless.
// - Budgets per extension and frame (`limits()`): vertices and indices drawn,
//   upload calls and upload bytes. An upload over budget throws RangeError
//   PXJS7801; a mesh that would draw past the vertex budget is skipped for
//   that frame and counted in `stats().skippedMeshes`.

import type {Color, Rect, RenderPhase, Unsubscribe} from "./index.js";

/** Floats per vertex in `uploadVertices`: x, y, u, v, r, g, b, a. */
export const RAW_VERTEX_STRIDE = 8;

/**
 * - normal: source-over alpha blending (non-premultiplied colors).
 * - premultiplied: source-over for premultiplied colors (render target contents).
 * - additive: adds the source, scaled by its alpha.
 * - multiply: multiplies the destination by the source color.
 * - replace: writes the source, alpha included, without blending.
 */
export type RawBlendMode = "normal" | "premultiplied" | "additive" | "multiply" | "replace";
export type RawFilter = "linear" | "nearest";

/** Where a mesh samples its texture coordinates. */
export type RawTextureSource =
  | {readonly kind: "asset"; readonly path: string}
  | {readonly kind: "texture"; readonly id: string}
  | {readonly kind: "target"; readonly id: string};

/** A 2x3 affine transform: points map to (a*x + c*y + tx, b*x + d*y + ty). */
export interface RawTransform {
  readonly a: number;
  readonly b: number;
  readonly c: number;
  readonly d: number;
  readonly tx: number;
  readonly ty: number;
}

export interface RawMeshOptions {
  /** When a screen mesh draws (default "after-stage"). Ignored with `target`. */
  readonly phase?: RenderPhase;
  /** Lower draws first within its phase or target. */
  readonly order?: number;
  readonly visible?: boolean;
  /** Empty or omitted means every route. */
  readonly route?: string;
  /** Null or omitted: vertex colors only. */
  readonly texture?: RawTextureSource | null;
  readonly blend?: RawBlendMode;
  /**
   * The id of another mesh whose coverage (its alpha, after its own texture
   * and vertex colors) masks this one. The mask mesh still draws by itself
   * unless it is not `visible`. Screen meshes only.
   */
  readonly mask?: string | null;
  /** Draw into this render target (each frame, before the screen) instead of the screen. */
  readonly target?: string | null;
  /** Applied to the vertex positions (logical pixels, or target pixels). */
  readonly transform?: RawTransform | null;
}

export interface RawTextureOptions {
  readonly filter?: RawFilter;
}

export interface RawTargetOptions {
  /** Cleared to this color (channels 0-255) every frame before its meshes draw; null keeps the contents. */
  readonly clear?: Color | null;
  /** Lower targets render first; a target sampled by another target's meshes should come first. */
  readonly order?: number;
  readonly filter?: RawFilter;
}

/** Pixel rectangle of a texture upload. */
export interface RawRegion {
  readonly x: number;
  readonly y: number;
  readonly w: number;
  readonly h: number;
}

export interface RawLimits {
  readonly maxVerticesPerFrame: number;
  readonly maxIndicesPerFrame: number;
  readonly maxUploadsPerFrame: number;
  readonly maxUploadBytesPerFrame: number;
  readonly maxVerticesPerMesh: number;
  readonly maxIndicesPerMesh: number;
  readonly maxMeshes: number;
  readonly maxTextures: number;
  readonly maxTargets: number;
  readonly maxTextureSize: number;
  /** The renderer behind SDL ("direct3d11", "metal", "opengl", "webgl2", "gpu", ...), for diagnostics only. */
  readonly backend: string;
}

/** This extension's counters for the frame in progress. */
export interface RawFrameStats {
  readonly frame: number;
  readonly vertices: number;
  readonly indices: number;
  readonly uploads: number;
  readonly uploadBytes: number;
  readonly skippedMeshes: number;
  readonly meshes: number;
  readonly textures: number;
  readonly targets: number;
}

/** The `EngineRaw` host object the Player installs (what `createPrismatiXRaw` wraps). */
export interface PrismatiXRawHost {
  Limits(): RawLimits;
  Stats(): RawFrameStats;
  CreateMesh(id: string, options?: RawMeshOptions): boolean;
  UpdateMesh(id: string, options: RawMeshOptions): boolean;
  /** Replaces the vertices, or with `firstVertex` overwrites that range of the existing ones. */
  UploadVertices(id: string, vertices: Float32Array, firstVertex?: number): void;
  /** Replaces the indices; omitted indices draw the vertices as a triangle list. */
  UploadIndices(id: string, indices: Uint16Array | Uint32Array): void;
  DestroyMesh(id: string): boolean;
  CreateTexture(id: string, width: number, height: number, options?: RawTextureOptions): boolean;
  /** RGBA8, rows top to bottom, `region.w * region.h * 4` bytes (the whole texture without a region). */
  UploadTexture(id: string, pixels: Uint8Array | Uint8ClampedArray, region?: RawRegion): void;
  DestroyTexture(id: string): boolean;
  CreateTarget(id: string, width: number, height: number, options?: RawTargetOptions): boolean;
  UpdateTarget(id: string, options: RawTargetOptions): boolean;
  DestroyTarget(id: string): boolean;
  /** Destroys every raw resource of this extension. */
  Clear(): void;
}

export interface RawMesh {
  readonly id: string;
  update(options: RawMeshOptions): boolean;
  vertices(data: Float32Array, firstVertex?: number): void;
  indices(data: Uint16Array | Uint32Array): void;
  destroy(): boolean;
}

export interface RawTexture {
  readonly id: string;
  readonly width: number;
  readonly height: number;
  /** As a mesh texture source. */
  readonly source: RawTextureSource;
  upload(pixels: Uint8Array | Uint8ClampedArray, region?: RawRegion): void;
  destroy(): boolean;
}

export interface RawTarget {
  readonly id: string;
  readonly width: number;
  readonly height: number;
  /** As a mesh texture source (its contents are premultiplied: draw it with blend "premultiplied"). */
  readonly source: RawTextureSource;
  update(options: RawTargetOptions): boolean;
  destroy(): boolean;
}

/** @experimental The layer-1 drawing API; see the module comment. */
export interface PrismatiXRaw {
  readonly host: PrismatiXRawHost;
  limits(): RawLimits;
  stats(): RawFrameStats;
  createMesh(id: string, options?: RawMeshOptions): RawMesh;
  mesh(id: string): RawMesh;
  createTexture(id: string, width: number, height: number, options?: RawTextureOptions): RawTexture;
  createTarget(id: string, width: number, height: number, options?: RawTargetOptions): RawTarget;
  /** A packaged image as a mesh texture source. */
  asset(path: string): RawTextureSource;
  /** Destroys every raw resource of this extension. */
  clear(): void;
  /**
   * Calls `draw` every frame (frame.update) until stopped; the usual place to
   * upload animated vertices. Needs only the capabilities `draw` uses.
   */
  onFrame(draw: (deltaSeconds: number) => void): Unsubscribe;
}

declare global {
  // Installed by the Player for every extension; each method checks the
  // manifest's raw.* capabilities.
  var EngineRaw: PrismatiXRawHost;
}

function meshHandle(host: PrismatiXRawHost, id: string): RawMesh {
  return Object.freeze({
    id,
    update: (options: RawMeshOptions) => host.UpdateMesh(id, options),
    vertices: (data: Float32Array, firstVertex?: number) => host.UploadVertices(id, data, firstVertex),
    indices: (data: Uint16Array | Uint32Array) => host.UploadIndices(id, data),
    destroy: () => host.DestroyMesh(id),
  });
}

/** @experimental Wraps the Player's `EngineRaw` (layer 1). */
export function createPrismatiXRaw(host: PrismatiXRawHost = globalThis.EngineRaw): PrismatiXRaw {
  return {
    host,
    limits: () => host.Limits(),
    stats: () => host.Stats(),
    createMesh: (id, options) => {
      host.CreateMesh(id, options);
      return meshHandle(host, id);
    },
    mesh: (id) => meshHandle(host, id),
    createTexture: (id, width, height, options) => {
      host.CreateTexture(id, width, height, options);
      return Object.freeze({
        id, width, height, source: Object.freeze({kind: "texture", id}) as RawTextureSource,
        upload: (pixels: Uint8Array | Uint8ClampedArray, region?: RawRegion) => host.UploadTexture(id, pixels, region),
        destroy: () => host.DestroyTexture(id),
      });
    },
    createTarget: (id, width, height, options) => {
      host.CreateTarget(id, width, height, options);
      return Object.freeze({
        id, width, height, source: Object.freeze({kind: "target", id}) as RawTextureSource,
        update: (next: RawTargetOptions) => host.UpdateTarget(id, next),
        destroy: () => host.DestroyTarget(id),
      });
    },
    asset: (path) => Object.freeze({kind: "asset", path}),
    clear: () => host.Clear(),
    onFrame: (draw) => {
      const engine = globalThis.Engine;
      let active = true;
      const listener: unknown = engine.on("frame.update", (payload) => {
        if (!active) return;
        const delta = Number((payload as {delta?: unknown} | null)?.delta);
        draw(Number.isFinite(delta) ? delta : 0);
      });
      return () => {
        if (!active) return;
        active = false;
        if (typeof listener === "number" && typeof engine.off === "function") engine.off(listener);
      };
    },
  };
}

/**
 * Interleaved vertices of an axis-aligned quad (4 vertices, use with
 * `QUAD_INDICES`). `uv` defaults to the whole texture, `color` (0-1) to white.
 */
export function quadVertices(bounds: Rect, uv: Rect = {x: 0, y: 0, w: 1, h: 1},
  color: readonly [number, number, number, number] = [1, 1, 1, 1]): Float32Array {
  const [r, g, b, a] = color;
  const right = bounds.x + bounds.w, bottom = bounds.y + bounds.h;
  const u1 = uv.x + uv.w, v1 = uv.y + uv.h;
  return new Float32Array([
    bounds.x, bounds.y, uv.x, uv.y, r, g, b, a,
    right, bounds.y, u1, uv.y, r, g, b, a,
    right, bottom, u1, v1, r, g, b, a,
    bounds.x, bottom, uv.x, v1, r, g, b, a,
  ]);
}

/** Two triangles over the four `quadVertices`. */
export const QUAD_INDICES: Uint16Array = new Uint16Array([0, 1, 2, 0, 2, 3]);
