import {open, readFile} from "node:fs/promises";
import {deflateSync, inflateSync} from "node:zlib";

/**
 * Just enough PNG/JPEG handling for importers: dimensions, the PNG/JPEG
 * comment text (Artemis stores sprite offsets there), and 8-bit PNG decode,
 * crop, downscale and encode for derived thumbnails and layer crops.
 */
export interface ImageHeader {
  readonly width: number;
  readonly height: number;
  /** PNG tEXt/zTXt/iTXt `comment`, or the first JPEG COM segment. */
  readonly comment?: string;
}

export interface RgbaImage {
  readonly width: number;
  readonly height: number;
  /** width * height * 4 bytes, straight alpha. */
  readonly pixels: Uint8Array;
}

const pngSignature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

interface PngChunk { readonly type: string; readonly data: Buffer }

function* pngChunks(bytes: Buffer): Generator<PngChunk> {
  let offset = 8;
  while (offset + 8 <= bytes.length) {
    const length = bytes.readUInt32BE(offset);
    const type = bytes.toString("latin1", offset + 4, offset + 8);
    const end = offset + 8 + length;
    if (end + 4 > bytes.length) throw new Error(`Truncated PNG chunk ${type}`);
    yield {type, data: bytes.subarray(offset + 8, end)};
    if (type === "IEND") return;
    offset = end + 4;
  }
}

function pngText(chunk: PngChunk): [string, string] | undefined {
  const zero = chunk.data.indexOf(0);
  if (zero < 0) return undefined;
  const keyword = chunk.data.toString("latin1", 0, zero);
  if (chunk.type === "tEXt") return [keyword, chunk.data.toString("latin1", zero + 1)];
  if (chunk.type === "zTXt")
    return [keyword, inflateSync(chunk.data.subarray(zero + 2)).toString("latin1")];
  if (chunk.type === "iTXt") {
    const compressed = chunk.data[zero + 1] === 1;
    let cursor = zero + 3;
    for (let field = 0; field < 2; field += 1) {
      const next = chunk.data.indexOf(0, cursor);
      if (next < 0) return undefined;
      cursor = next + 1;
    }
    const body = chunk.data.subarray(cursor);
    return [keyword, (compressed ? inflateSync(body) : body).toString("utf8")];
  }
  return undefined;
}

function jpegHeader(bytes: Buffer, path: string): ImageHeader {
  let offset = 2;
  let comment: string | undefined;
  while (offset + 4 <= bytes.length) {
    if (bytes[offset] !== 0xff) { offset += 1; continue; }
    const marker = bytes[offset + 1]!;
    if (marker === 0xff) { offset += 1; continue; }
    if (marker === 0xd8 || marker === 0x01 || (marker >= 0xd0 && marker <= 0xd7)) { offset += 2; continue; }
    const length = bytes.readUInt16BE(offset + 2);
    if (marker === 0xfe && comment === undefined)
      comment = bytes.toString("latin1", offset + 4, offset + 2 + length);
    if (marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc)
      return {height: bytes.readUInt16BE(offset + 5), width: bytes.readUInt16BE(offset + 7),
        ...(comment === undefined ? {} : {comment})};
    offset += 2 + length;
  }
  throw new Error(`No JPEG frame header in ${path}`);
}

export function imageHeaderFromBytes(bytes: Buffer, path = "image"): ImageHeader {
  if (bytes.subarray(0, 8).equals(pngSignature)) {
    let width = 0;
    let height = 0;
    let comment: string | undefined;
    for (const chunk of pngChunks(bytes)) {
      if (chunk.type === "IHDR") { width = chunk.data.readUInt32BE(0); height = chunk.data.readUInt32BE(4); }
      else if (chunk.type === "IDAT") break;
      else if (comment === undefined && (chunk.type === "tEXt" || chunk.type === "zTXt" || chunk.type === "iTXt")) {
        const text = pngText(chunk);
        if (text?.[0] === "comment") comment = text[1];
      }
    }
    return {width, height, ...(comment === undefined ? {} : {comment})};
  }
  if (bytes[0] === 0xff && bytes[1] === 0xd8) return jpegHeader(bytes, path);
  throw new Error(`Unsupported image format: ${path}`);
}

/** Dimensions and comment, reading only the file's head when that suffices. */
export async function readImageHeader(path: string): Promise<ImageHeader> {
  const handle = await open(path, "r");
  try {
    const head = Buffer.alloc(65536);
    const {bytesRead} = await handle.read(head, 0, head.length, 0);
    try { return imageHeaderFromBytes(head.subarray(0, bytesRead), path); }
    catch { return imageHeaderFromBytes(await readFile(path), path); }
  } finally { await handle.close(); }
}

function paeth(left: number, up: number, upperLeft: number): number {
  const estimate = left + up - upperLeft;
  const a = Math.abs(estimate - left);
  const b = Math.abs(estimate - up);
  const c = Math.abs(estimate - upperLeft);
  return a <= b && a <= c ? left : b <= c ? up : upperLeft;
}

/** Decode a non-interlaced 8-bit (or 16-bit, reduced) PNG to RGBA. */
export function decodePng(bytes: Buffer, path = "image"): RgbaImage {
  if (!bytes.subarray(0, 8).equals(pngSignature)) throw new Error(`Not a PNG: ${path}`);
  let width = 0, height = 0, depth = 0, colorType = 0, interlace = 0;
  let palette: Buffer | undefined;
  let transparency: Buffer | undefined;
  const data: Buffer[] = [];
  for (const chunk of pngChunks(bytes)) {
    if (chunk.type === "IHDR") {
      width = chunk.data.readUInt32BE(0); height = chunk.data.readUInt32BE(4);
      depth = chunk.data[8]!; colorType = chunk.data[9]!; interlace = chunk.data[12]!;
    } else if (chunk.type === "PLTE") palette = chunk.data;
    else if (chunk.type === "tRNS") transparency = chunk.data;
    else if (chunk.type === "IDAT") data.push(chunk.data);
  }
  const channels = ({0: 1, 2: 3, 3: 1, 4: 2, 6: 4} as Record<number, number>)[colorType];
  if (channels === undefined || interlace !== 0 || (depth !== 8 && depth !== 16) ||
      (colorType === 3 && depth !== 8))
    throw new Error(`Unsupported PNG layout in ${path} (type ${colorType}, depth ${depth}, interlace ${interlace})`);
  const bytesPerPixel = channels * (depth / 8);
  const stride = width * bytesPerPixel;
  const raw = inflateSync(Buffer.concat(data));
  const rows = Buffer.alloc(stride * height);
  for (let y = 0; y < height; y += 1) {
    const filter = raw[y * (stride + 1)]!;
    const source = y * (stride + 1) + 1;
    const target = y * stride;
    for (let x = 0; x < stride; x += 1) {
      const value = raw[source + x]!;
      const left = x >= bytesPerPixel ? rows[target + x - bytesPerPixel]! : 0;
      const up = y > 0 ? rows[target - stride + x]! : 0;
      const upperLeft = y > 0 && x >= bytesPerPixel ? rows[target - stride + x - bytesPerPixel]! : 0;
      rows[target + x] = (value + (filter === 1 ? left : filter === 2 ? up :
        filter === 3 ? (left + up) >> 1 : filter === 4 ? paeth(left, up, upperLeft) : 0)) & 0xff;
    }
  }
  const pixels = new Uint8Array(width * height * 4);
  const sample = (offset: number) => depth === 16 ? rows[offset * 2]! : rows[offset]!;
  for (let index = 0; index < width * height; index += 1) {
    const base = index * channels;
    let r: number, g: number, b: number, a = 255;
    if (colorType === 0) {
      r = g = b = sample(base);
      if (transparency !== undefined && transparency.length >= 2 && depth === 8 &&
          r === transparency.readUInt16BE(0)) a = 0;
    } else if (colorType === 2) {
      r = sample(base); g = sample(base + 1); b = sample(base + 2);
    } else if (colorType === 3) {
      const entry = rows[index]!;
      r = palette?.[entry * 3] ?? 0; g = palette?.[entry * 3 + 1] ?? 0; b = palette?.[entry * 3 + 2] ?? 0;
      a = transparency?.[entry] ?? 255;
    } else if (colorType === 4) {
      r = g = b = sample(base); a = sample(base + 1);
    } else {
      r = sample(base); g = sample(base + 1); b = sample(base + 2); a = sample(base + 3);
    }
    pixels.set([r, g, b, a], index * 4);
  }
  return {width, height, pixels};
}

const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(bytes: Buffer): number {
  let crc = 0xffffffff;
  for (const byte of bytes) crc = crcTable[(crc ^ byte) & 0xff]! ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Buffer): Buffer {
  const header = Buffer.alloc(8);
  header.writeUInt32BE(data.length, 0);
  header.write(type, 4, "latin1");
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([header.subarray(4), data])), 0);
  return Buffer.concat([header, data, crc]);
}

/** Encode RGBA as an 8-bit PNG (up filter per row). */
export function encodePng(image: RgbaImage): Buffer {
  const stride = image.width * 4;
  const raw = Buffer.alloc((stride + 1) * image.height);
  for (let y = 0; y < image.height; y += 1) {
    raw[y * (stride + 1)] = 2;
    for (let x = 0; x < stride; x += 1) {
      const value = image.pixels[y * stride + x]!;
      const up = y > 0 ? image.pixels[(y - 1) * stride + x]! : 0;
      raw[y * (stride + 1) + 1 + x] = (value - up) & 0xff;
    }
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(image.width, 0);
  header.writeUInt32BE(image.height, 4);
  header[8] = 8; header[9] = 6;
  return Buffer.concat([pngSignature, chunk("IHDR", header),
    chunk("IDAT", deflateSync(raw, {level: 9})), chunk("IEND", Buffer.alloc(0))]);
}

/** The top-left `width` x `height` region (clamped to the image). */
export function cropImage(image: RgbaImage, width: number, height: number): RgbaImage {
  const w = Math.max(0, Math.min(width, image.width));
  const h = Math.max(0, Math.min(height, image.height));
  const pixels = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y += 1)
    pixels.set(image.pixels.subarray(y * image.width * 4, (y * image.width + w) * 4), y * w * 4);
  return {width: w, height: h, pixels};
}

function lanczos(x: number): number {
  if (x === 0) return 1;
  if (x <= -3 || x >= 3) return 0;
  const px = Math.PI * x;
  return 3 * Math.sin(px) * Math.sin(px / 3) / (px * px);
}

function resampleAxis(source: Float64Array, width: number, height: number,
  target: number, horizontal: boolean): Float64Array {
  const length = horizontal ? width : height;
  const other = horizontal ? height : width;
  const scale = length / target;
  const support = 3 * Math.max(1, scale);
  const output = new Float64Array((horizontal ? target * height : width * target) * 4);
  for (let t = 0; t < target; t += 1) {
    const center = (t + 0.5) * scale;
    const first = Math.max(0, Math.floor(center - support));
    const last = Math.min(length - 1, Math.ceil(center + support));
    const weights: number[] = [];
    let total = 0;
    for (let s = first; s <= last; s += 1) {
      const weight = lanczos((s + 0.5 - center) / Math.max(1, scale));
      weights.push(weight); total += weight;
    }
    for (let o = 0; o < other; o += 1) {
      const sums = [0, 0, 0, 0];
      for (let s = first; s <= last; s += 1) {
        const weight = weights[s - first]! / total;
        const index = (horizontal ? o * width + s : s * width + o) * 4;
        // Premultiplied so transparent pixels do not bleed their colour.
        const alpha = source[index + 3]!;
        sums[0]! += source[index]! * alpha * weight;
        sums[1]! += source[index + 1]! * alpha * weight;
        sums[2]! += source[index + 2]! * alpha * weight;
        sums[3]! += alpha * weight;
      }
      const index = (horizontal ? o * target + t : t * width + o) * 4;
      const alpha = sums[3]!;
      output[index] = alpha > 0 ? sums[0]! / alpha : 0;
      output[index + 1] = alpha > 0 ? sums[1]! / alpha : 0;
      output[index + 2] = alpha > 0 ? sums[2]! / alpha : 0;
      output[index + 3] = alpha;
    }
  }
  return output;
}

/** Fit inside `maxWidth` x `maxHeight` keeping the aspect ratio (Lanczos-3). */
export function thumbnailImage(image: RgbaImage, maxWidth: number, maxHeight: number): RgbaImage {
  if (image.width <= maxWidth && image.height <= maxHeight) return image;
  const ratio = Math.min(maxWidth / image.width, maxHeight / image.height);
  const width = Math.max(1, Math.round(image.width * ratio));
  const height = Math.max(1, Math.round(image.height * ratio));
  const source = Float64Array.from(image.pixels);
  const horizontal = resampleAxis(source, image.width, image.height, width, true);
  const vertical = resampleAxis(horizontal, width, image.height, height, false);
  const pixels = new Uint8Array(width * height * 4);
  for (let index = 0; index < pixels.length; index += 1)
    pixels[index] = Math.max(0, Math.min(255, Math.round(vertical[index]!)));
  return {width, height, pixels};
}
