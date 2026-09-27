// FNV-1a 64-bit, split into two unsigned 32-bit limbs. Keeping the exact
// historical bytes and output preserves Story operation and asset identities.
// Every intermediate product is below 2^53, so Number arithmetic is exact.
const encoder = new TextEncoder();
const lowPrime = 0x1b3;
const highPrime = 0x100;
const limb = 0x1_0000_0000;

export function fnv1a64(input: string): string {
  let high = 0xcbf29ce4;
  let low = 0x84222325;
  for (const byte of encoder.encode(input)) {
    low = (low ^ byte) >>> 0;
    const lowProduct = low * lowPrime;
    high = (high * lowPrime + low * highPrime +
      Math.floor(lowProduct / limb)) >>> 0;
    low = lowProduct >>> 0;
  }
  return high.toString(16).padStart(8, "0") +
    low.toString(16).padStart(8, "0");
}
