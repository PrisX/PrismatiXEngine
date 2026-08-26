import assert from "node:assert/strict";
import test from "node:test";

import {
  assetIdFromToken,
  assetToken,
  isAssetToken,
  resolveResourceReference,
  resourceReferenceKind,
  stableResourceReference,
} from "../src/index.js";

const id = "33333333-3333-4333-8333-333333333333";
const asset = {
  id,
  source: "Assets/Characters/yuki/smile.webp",
  kind: "character",
  name: "Yuki Smile",
} as const;

test("resource identity helpers keep ordinary authored paths path-first", () => {
  assert.equal(resourceReferenceKind("Assets/Characters/yuki/smile.webp"), "path");
  assert.equal(isAssetToken("Assets/Characters/yuki/smile.webp"), false);
  assert.deepEqual(resolveResourceReference("Assets/Characters/yuki/smile.webp", [asset]), {
    kind: "path",
    path: "Assets/Characters/yuki/smile.webp",
  });
});

test("assetToken creates the canonical stable-identity Runtime token", () => {
  assert.equal(assetToken(id.toUpperCase()), `asset:${id}`);
  assert.equal(assetIdFromToken(`asset:${id.toUpperCase()}`), id);
  assert.equal(isAssetToken(`asset:${id}`), true);
  assert.equal(resourceReferenceKind(`asset:${id}`), "asset");
  assert.equal(stableResourceReference(asset), `asset:${id}`);
});

test("resolveResourceReference resolves catalog identities without changing authored paths", () => {
  assert.deepEqual(resolveResourceReference(`asset:${id}`, [asset]), {
    kind: "asset",
    assetId: id,
    path: asset.source,
    asset,
  });
});

test("resolveResourceReference preserves an unresolved stable identity for repair tooling", () => {
  const missingId = "44444444-4444-4444-8444-444444444444";
  assert.deepEqual(resolveResourceReference(`asset:${missingId}`, [asset]), {
    kind: "asset",
    assetId: missingId,
  });
});

test("assetToken rejects non-UUID identities", () => {
  assert.throws(() => assetToken("yuki-smile"), /UUID/u);
  assert.equal(assetIdFromToken("asset:yuki-smile"), undefined);
  assert.equal(isAssetToken("asset:yuki-smile"), false);
});
