// The sandbox has no atob. A .moc3 is binary and scripts read resources as
// text only (`ctx.assets.readText`), so the model ships its .moc3 as base64
// text (tools/moc-to-text.mjs) and this turns it back into bytes.

const ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const LOOKUP = new Int8Array(128).fill(-1);
for (let index = 0; index < ALPHABET.length; index += 1) LOOKUP[ALPHABET.charCodeAt(index)] = index;
// base64url as well, in case a tool wrote it.
LOOKUP["-".charCodeAt(0)] = 62;
LOOKUP["_".charCodeAt(0)] = 63;

/** Decodes base64 (padding and whitespace optional); anything else throws. */
export function decodeBase64(text: string): Uint8Array {
  let digits = 0;
  for (let index = 0; index < text.length; index += 1) {
    const code = text.charCodeAt(index);
    if (code === 61 || code === 10 || code === 13 || code === 32 || code === 9) continue;
    if (code >= 128 || LOOKUP[code]! < 0) throw new Error(`Live2D: not base64 at character ${index}`);
    digits += 1;
  }
  const bytes = new Uint8Array(Math.floor((digits * 6) / 8));
  let accumulator = 0, bits = 0, written = 0;
  for (let index = 0; index < text.length && written < bytes.length; index += 1) {
    const code = text.charCodeAt(index);
    if (code === 61 || code === 10 || code === 13 || code === 32 || code === 9) continue;
    accumulator = (accumulator << 6) | LOOKUP[code]!;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes[written++] = (accumulator >> bits) & 0xff;
      accumulator &= (1 << bits) - 1;
    }
  }
  return bytes;
}
