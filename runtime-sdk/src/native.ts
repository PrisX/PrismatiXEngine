// @prismatix/runtime/native: talk to the extension's native services (layer 2).
//
// A native extension ships signed C ABI libraries (docs/native-extensions.md)
// that register named services with the Player. Scripts never see a function
// pointer: they send a service a request of bytes and get bytes back, and
// can receive messages the service posts. The protocol inside the bytes is
// the extension's own (JSON, MessagePack, FlatBuffers, ...), so another
// platform can provide the same service another way without the script
// changing. The Web Player loads no native libraries, so there `has()` is
// false and `request()` throws.
//
// Needs the manifest's "native" capability; a script may call only the
// services its manifest lists in `native.services`.

import type {Unsubscribe} from "./index.js";

/** The `EngineNative` host object the Player installs (what `createPrismatiXNative` wraps). */
export interface PrismatiXNativeHost {
  /** The services this extension's manifest declares that a loaded library registered. */
  Services(): readonly string[];
  Has(service: string): boolean;
  /**
   * Calls the service synchronously on the game thread. Throws PXJS7901 when
   * the service is not available here and PXJS7902 when it fails. Requests and
   * responses are at most 16 MiB. Inside a replayed handler (a load or
   * rollback) the recorded response is returned without calling the library.
   */
  Request(service: string, request: Uint8Array): Uint8Array;
  /** Messages the service posted since the last call, oldest first. */
  TakeMessages(service: string): readonly Uint8Array[];
}

/** Bytes, or text sent as UTF-8. */
export type NativePayload = Uint8Array | string;

export interface PrismatiXNative {
  readonly host: PrismatiXNativeHost;
  services(): readonly string[];
  has(service: string): boolean;
  request(service: string, payload?: NativePayload): Uint8Array;
  requestText(service: string, payload?: NativePayload): string;
  /** Sends `JSON.stringify(value)` and parses the response as JSON. */
  requestJson<T = unknown>(service: string, value?: unknown): T;
  /** Delivers the service's messages every frame until stopped. */
  onMessage(service: string, handler: (message: Uint8Array) => void): Unsubscribe;
}

declare global {
  // Installed by the Player for every extension; each method checks the
  // manifest's "native" capability and `native.services`.
  var EngineNative: PrismatiXNativeHost;
}

/** UTF-8 encoding (the sandbox has no TextEncoder). */
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

/** UTF-8 decoding; malformed sequences become U+FFFD. */
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
    if (valid && (length === 3 && (code < 0x800 || (code >= 0xd800 && code <= 0xdfff)) || length === 4 && (code < 0x10000 || code > 0x10ffff)))
      valid = false;
    text += valid ? String.fromCodePoint(code) : "�";
    index += valid ? length : 1;
  }
  return text;
}

function bytesOf(payload: NativePayload | undefined): Uint8Array {
  return payload === undefined ? new Uint8Array(0) : typeof payload === "string" ? encodeUtf8(payload) : payload;
}

/** Wraps the Player's `EngineNative` (layer 2). */
export function createPrismatiXNative(host: PrismatiXNativeHost = globalThis.EngineNative): PrismatiXNative {
  const request = (service: string, payload?: NativePayload) => host.Request(service, bytesOf(payload));
  return {
    host,
    services: () => host.Services(),
    has: (service) => host.Has(service),
    request,
    requestText: (service, payload) => decodeUtf8(request(service, payload)),
    requestJson: <T>(service: string, value?: unknown) => {
      const response = decodeUtf8(request(service, value === undefined ? "" : JSON.stringify(value)));
      return (response === "" ? undefined : JSON.parse(response)) as T;
    },
    onMessage: (service, handler) => {
      const engine = globalThis.Engine;
      let active = true;
      const listener: unknown = engine.on("frame.update", () => {
        if (!active) return;
        for (const message of host.TakeMessages(service)) {
          if (!active) break;
          handler(message);
        }
      });
      return () => {
        if (!active) return;
        active = false;
        if (typeof listener === "number" && typeof engine.off === "function") engine.off(listener);
      };
    },
  };
}
