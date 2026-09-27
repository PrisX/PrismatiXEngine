// FNV-1a 64-bit, split into two unsigned 32-bit limbs. Keeping the exact
// historical bytes and output preserves Story operation and asset identities.
// Every intermediate product is below 2^53, so Number arithmetic is exact.
const encoder = new TextEncoder();
const utf8Chunk = new Uint8Array(4096);
const lowPrime = 0x1b3;
const highPrime = 0x100;
const limb = 0x1_0000_0000;

export function fnv1a64(input: string): string {
  let high = 0xcbf29ce4;
  let low = 0x84222325;
  // Most generated operation identities are ASCII paths and UUIDs. Their UTF-8
  // bytes equal the UTF-16 code units, so avoid an encoder call per identity.
  let offset = 0;
  for (; offset < input.length; offset += 1) {
    const code = input.charCodeAt(offset);
    if (code > 0x7f) break;
    low = (low ^ code) >>> 0;
    const lowProduct = low * lowPrime;
    high = (high * lowPrime + low * highPrime +
      Math.floor(lowProduct / limb)) >>> 0;
    low = lowProduct >>> 0;
  }
  if (offset < input.length && typeof encoder.encodeInto === "function") {
    // Avoid allocating one Uint8Array per ID. encodeInto consumes whole Unicode
    // scalars, so a surrogate pair at a chunk boundary keeps its old UTF-8 bytes.
    for (; offset < input.length;) {
      const {read, written} = encoder.encodeInto(input.slice(offset), utf8Chunk);
      if (read === 0) throw new Error("UTF-8 encoder made no progress");
      offset += read;
      for (let index = 0; index < written; index += 1) {
        low = (low ^ utf8Chunk[index]!) >>> 0;
        const lowProduct = low * lowPrime;
        high = (high * lowPrime + low * highPrime +
          Math.floor(lowProduct / limb)) >>> 0;
        low = lowProduct >>> 0;
      }
    }
  } else if (offset < input.length) {
    // Editors with an older TextEncoder retain the historical encoding path.
    for (const byte of encoder.encode(input.slice(offset))) {
      low = (low ^ byte) >>> 0;
      const lowProduct = low * lowPrime;
      high = (high * lowPrime + low * highPrime +
        Math.floor(lowProduct / limb)) >>> 0;
      low = lowProduct >>> 0;
    }
  }
  return high.toString(16).padStart(8, "0") +
    low.toString(16).padStart(8, "0");
}
