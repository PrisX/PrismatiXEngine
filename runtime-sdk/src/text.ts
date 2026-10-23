// @prismatix/runtime/text: bytes and text without TextEncoder, atob or btoa.
//
// The Player's sandbox has none of the Web text APIs, so extensions that read
// binary resources (`ctx.assets.readBytes`) or talk to native services in
// bytes use these. They are plain functions: no capability, no host object.

/** UTF-8 encoding; lone surrogates become U+FFFD. */
export function encodeUtf8(text: string): Uint8Array {
  const bytes: number[] = [];
  for (const character of text) {
    let code = character.codePointAt(0)!;
    if (code >= 0xd800 && code <= 0xdfff) code = 0xfffd;
    if (code < 0x80) bytes.push(code);
    else if (code < 0x800) bytes.push(0xc0 | (code >> 6), 0x80 | (code & 63));
    else if (code < 0x10000) bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
    else bytes.push(0xf0 | (code >> 18), 0x80 | ((code >> 12) & 63), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
  }
  return new Uint8Array(bytes);
}

/** UTF-8 decoding; malformed sequences become U+FFFD. A leading byte order mark is kept. */
export function decodeUtf8(bytes: Uint8Array): string {
  const parts: string[] = [];
  for (let index = 0; index < bytes.length;) {
    const first = bytes[index]!;
    const length = first < 0x80 ? 1 : first >= 0xc2 && first < 0xe0 ? 2 : first >= 0xe0 && first < 0xf0 ? 3
      : first >= 0xf0 && first < 0xf5 ? 4 : 0;
    let code = length === 1 ? first : length === 2 ? first & 31 : length === 3 ? first & 15 : first & 7;
    let valid = length > 0 && index + length <= bytes.length;
    for (let offset = 1; valid && offset < length; offset += 1) {
      const next = bytes[index + offset]!;
      if ((next & 0xc0) !== 0x80) valid = false;
      code = (code << 6) | (next & 63);
    }
    if (valid && (length === 3 && (code < 0x800 || (code >= 0xd800 && code <= 0xdfff)) || length === 4 && (code < 0x10000 || code > 0x10ffff)))
      valid = false;
    parts.push(valid ? String.fromCodePoint(code) : "�");
    index += valid ? length : 1;
  }
  return parts.join("");
}

const BASE64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const BASE64_VALUES = new Map<string, number>([...BASE64].map((character, value) => [character, value]));
BASE64_VALUES.set("-", 62).set("_", 63);

/** Standard base64 with padding (RFC 4648 section 4); `url` uses - and _ without padding. */
export function toBase64(bytes: Uint8Array, url = false): string {
  const parts: string[] = [];
  for (let index = 0; index < bytes.length; index += 3) {
    const a = bytes[index]!, b = bytes[index + 1] ?? 0, c = bytes[index + 2] ?? 0;
    const remaining = bytes.length - index;
    parts.push(BASE64[a >> 2]!, BASE64[((a & 3) << 4) | (b >> 4)]!,
      remaining > 1 ? BASE64[((b & 15) << 2) | (c >> 6)]! : "=", remaining > 2 ? BASE64[c & 63]! : "=");
  }
  const text = parts.join("");
  return url ? text.replace(/=+$/u, "").replaceAll("+", "-").replaceAll("/", "_") : text;
}

/** Decodes standard or URL-safe base64; whitespace is ignored and padding optional. Throws TypeError on anything else. */
export function fromBase64(text: string): Uint8Array {
  const clean = text.replace(/\s+/gu, "").replace(/={1,2}$/u, "");
  if (clean.length % 4 === 1) throw new TypeError("base64 text has a stray character at the end");
  const bytes = new Uint8Array(Math.floor(clean.length * 3 / 4));
  let written = 0, buffer = 0, bits = 0;
  for (const character of clean) {
    const value = BASE64_VALUES.get(character);
    if (value === undefined) throw new TypeError(`not a base64 character: ${JSON.stringify(character)}`);
    buffer = (buffer << 6) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes[written++] = (buffer >> bits) & 255;
    }
  }
  return bytes;
}

/** Lowercase hexadecimal, two digits per byte. */
export function toHex(bytes: Uint8Array): string {
  const parts: string[] = [];
  for (const byte of bytes) parts.push(byte.toString(16).padStart(2, "0"));
  return parts.join("");
}

/** Decodes hexadecimal (either case). Throws TypeError on an odd length or a non-hex digit. */
export function fromHex(text: string): Uint8Array {
  if (text.length % 2 !== 0 || !/^[0-9a-fA-F]*$/u.test(text)) throw new TypeError("hex text needs pairs of hex digits");
  const bytes = new Uint8Array(text.length / 2);
  for (let index = 0; index < bytes.length; index += 1) bytes[index] = Number.parseInt(text.slice(index * 2, index * 2 + 2), 16);
  return bytes;
}
