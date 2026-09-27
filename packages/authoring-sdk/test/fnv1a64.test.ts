import assert from "node:assert/strict";
import test from "node:test";

import {fnv1a64} from "../src/fnv1a64.js";

// This is the pre-optimization algorithm. Any change here would alter saved
// Story operation IDs, sidecar fingerprints, and normalized asset UUIDs.
function legacyHash(input: string): string {
  let hash = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(input)) {
    hash ^= BigInt(byte);
    hash = BigInt.asUintN(64, hash * 0x100000001b3n);
  }
  return hash.toString(16).padStart(16, "0");
}

test("stable 64-bit IDs remain byte-for-byte compatible with legacy hashes", () => {
  const examples = ["", "a", "Story/scene-001.pxstory:日本語", "雪🌸台詞",
    "operation\0dialogue\0line", "a".repeat(100_000), "\uD800"];
  for (const example of examples)
    assert.equal(fnv1a64(example), legacyHash(example));

  let state = 0x12345678;
  for (let index = 0; index < 1_000; index += 1) {
    let value = "";
    const length = index % 131;
    for (let position = 0; position < length; position += 1) {
      state = (Math.imul(state, 1_664_525) + 1_013_904_223) >>> 0;
      value += String.fromCodePoint([state % 0x80,
        0x300 + state % 0x200, 0x4e00 + state % 0x500,
        0x1f300 + state % 0x200][position % 4]!);
    }
    assert.equal(fnv1a64(value), legacyHash(value), `sample ${index}`);
  }
});
