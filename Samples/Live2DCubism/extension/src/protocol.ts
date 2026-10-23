// The bytes the native service speaks. native/cubism_bridge.h is the
// authoritative description; this is its script-side encoder and decoder.
// Everything is little-endian.

/** Core's constant flags of a drawable. */
export const BLEND_ADDITIVE = 1;
export const BLEND_MULTIPLICATIVE = 2;
export const INVERTED_MASK = 8;

/** Core's dynamic flags of a drawable, as the update response reports them. */
export const DYNAMIC_VISIBLE = 1;
export const DYNAMIC_VISIBILITY_CHANGED = 2;
export const DYNAMIC_OPACITY_CHANGED = 4;
export const DYNAMIC_DRAW_ORDER_CHANGED = 8;
export const DYNAMIC_RENDER_ORDER_CHANGED = 16;
export const DYNAMIC_POSITIONS_CHANGED = 32;
/** The flags after which the response carries the drawable's vertices. */
export const VERTICES_CHANGED = DYNAMIC_POSITIONS_CHANGED | DYNAMIC_OPACITY_CHANGED;
/** What changes how the drawables stack or which of them draw. */
export const STRUCTURE_CHANGED =
  DYNAMIC_VISIBILITY_CHANGED | DYNAMIC_DRAW_ORDER_CHANGED | DYNAMIC_RENDER_ORDER_CHANGED;

/** Floats per vertex: x, y, u, v, r, g, b, a (the Player's raw mesh layout). */
export const VERTEX_FLOATS = 8;
export const UPDATE_FORCE_ALL = 1;

export interface CanvasInfo {
  readonly w: number;
  readonly h: number;
  readonly ox: number;
  readonly oy: number;
  readonly ppu: number;
}

export interface ParameterInfo {
  readonly id: string;
  readonly min: number;
  readonly max: number;
  readonly def: number;
}

export interface DrawableInfo {
  readonly id: string;
  readonly texture: number;
  /** Core's constant flags. */
  readonly flags: number;
  /** Vertex and index counts. */
  readonly vc: number;
  readonly ic: number;
  /** Indices of the drawables that clip this one. */
  readonly masks: readonly number[];
}

export interface ModelDescription {
  readonly coreVersion: number;
  readonly canvas: CanvasInfo;
  readonly parameters: readonly ParameterInfo[];
  readonly parts: readonly string[];
  readonly drawables: readonly DrawableInfo[];
  /** Triangle list per drawable, in the order of `drawables`. */
  readonly indices: readonly Uint16Array[];
}

export interface CreatedModel {
  readonly handle: number;
  readonly description: ModelDescription;
}

export interface UpdateOptions {
  readonly handle: number;
  readonly forceAll: boolean;
  /** x' = ax * x + tx, y' = ay * y + ty: model units to screen pixels. */
  readonly transform: readonly [ax: number, ay: number, tx: number, ty: number];
  readonly parameters: ArrayLike<number>;
  readonly partOpacities?: ArrayLike<number>;
}

export interface UpdateFrame {
  /** Dynamic flags per drawable. */
  readonly flags: Uint8Array;
  readonly renderOrders: Int32Array;
  /** Interleaved vertices of the drawables that changed, null for the rest. */
  readonly vertices: readonly (Float32Array | null)[];
}

export function decodeUtf8(bytes: Uint8Array): string {
  let text = "";
  for (let index = 0; index < bytes.length;) {
    const first = bytes[index]!;
    const length = first < 0x80 ? 1 : first >= 0xf0 && first < 0xf5 ? 4 : first >= 0xe0 ? 3 : first >= 0xc2 && first < 0xe0 ? 2 : 0;
    let code = length === 1 ? first : length === 2 ? first & 31 : length === 3 ? first & 15 : first & 7;
    let valid = length > 0 && index + length <= bytes.length;
    for (let offset = 1; valid && offset < length; offset += 1) {
      const next = bytes[index + offset]!;
      if ((next & 0xc0) !== 0x80) valid = false;
      code = (code << 6) | (next & 63);
    }
    text += valid ? String.fromCodePoint(code) : "�";
    index += valid ? length : 1;
  }
  return text;
}

export function encodeUtf8(text: string): Uint8Array {
  const bytes: number[] = [];
  for (const character of text) {
    const code = character.codePointAt(0)!;
    if (code < 0x80) bytes.push(code);
    else if (code < 0x800) bytes.push(0xc0 | (code >> 6), 0x80 | (code & 63));
    else if (code < 0x10000) bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
    else bytes.push(0xf0 | (code >> 18), 0x80 | ((code >> 12) & 63), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
  }
  return new Uint8Array(bytes);
}

function view(bytes: Uint8Array): DataView {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

/** Every response starts with u32 status; non-zero is followed by a message. */
function expectOk(bytes: Uint8Array): void {
  if (bytes.length < 4) throw new Error("Live2D: the native service answered with fewer than 4 bytes");
  if (view(bytes).getUint32(0, true) !== 0) throw new Error(`Live2D: ${decodeUtf8(bytes.subarray(4))}`);
}

/** Typed views need aligned offsets; a response normally starts at 0, a slice may not. */
function aligned(bytes: Uint8Array): Uint8Array {
  return bytes.byteOffset % 4 === 0 ? bytes : bytes.slice();
}

export function decodeCoreLoaded(bytes: Uint8Array): number {
  expectOk(bytes);
  return (JSON.parse(decodeUtf8(bytes.subarray(4))) as {coreVersion: number}).coreVersion;
}

export function decodeCreate(bytes: Uint8Array): CreatedModel {
  expectOk(bytes);
  const data = view(bytes);
  const handle = data.getUint32(4, true);
  const jsonLength = data.getUint32(8, true);
  const json = JSON.parse(decodeUtf8(bytes.subarray(12, 12 + jsonLength))) as Omit<ModelDescription, "indices">;
  let offset = (12 + jsonLength + 3) & ~3;
  const source = aligned(bytes);
  const indices = json.drawables.map((drawable) => {
    const view16 = new Uint16Array(source.buffer, source.byteOffset + offset, drawable.ic);
    offset += drawable.ic * 2;
    return view16;
  });
  return {handle, description: {...json, indices}};
}

export function encodeUpdate(options: UpdateOptions): Uint8Array {
  const parts = options.partOpacities ?? [];
  const bytes = new Uint8Array(4 + 4 + 16 + 4 + options.parameters.length * 4 + 4 + parts.length * 4);
  const data = new DataView(bytes.buffer);
  let at = 0;
  data.setUint32(at, options.handle, true); at += 4;
  data.setUint32(at, options.forceAll ? UPDATE_FORCE_ALL : 0, true); at += 4;
  for (const value of options.transform) { data.setFloat32(at, value, true); at += 4; }
  data.setUint32(at, options.parameters.length, true); at += 4;
  for (let index = 0; index < options.parameters.length; index += 1) { data.setFloat32(at, options.parameters[index]!, true); at += 4; }
  data.setUint32(at, parts.length, true); at += 4;
  for (let index = 0; index < parts.length; index += 1) { data.setFloat32(at, parts[index]!, true); at += 4; }
  return bytes;
}

export function decodeUpdate(bytes: Uint8Array, description: ModelDescription): UpdateFrame {
  expectOk(bytes);
  const count = view(bytes).getUint32(4, true);
  if (count !== description.drawables.length)
    throw new Error(`Live2D: the update covers ${count} drawables, the model has ${description.drawables.length}`);
  const source = aligned(bytes);
  const flags = source.subarray(8, 8 + count);
  let offset = (8 + count + 3) & ~3;
  const renderOrders = new Int32Array(source.buffer, source.byteOffset + offset, count);
  offset += count * 4;
  const vertices = description.drawables.map((drawable, index) => {
    if ((flags[index]! & VERTICES_CHANGED) === 0) return null;
    const floats = new Float32Array(source.buffer, source.byteOffset + offset, drawable.vc * VERTEX_FLOATS);
    offset += floats.byteLength;
    return floats;
  });
  if (offset !== source.length) throw new Error(`Live2D: the update has ${source.length - offset} unread bytes`);
  return {flags, renderOrders, vertices};
}
