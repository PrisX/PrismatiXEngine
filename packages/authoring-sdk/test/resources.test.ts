import assert from "node:assert/strict";
import test from "node:test";

import {assetToken, isAssetToken, resourceReferenceKind} from "../src/index.js";

const id = "33333333-3333-4333-8333-333333333333";

test("resource identity helpers keep ordinary authored paths path-first", () => {
  assert.equal(resourceReferenceKind("Assets/Characters/yuki/smile.webp"), "path");
  assert.equal(isAssetToken("Assets/Characters/yuki/smile.webp"), false);
});

test("assetToken creates the canonical stable-identity Runtime token", () => {
  assert.equal(assetToken(id.toUpperCase()), `asset:${id}`);
  assert.equal(isAssetToken(`asset:${id}`), true);
  assert.equal(resourceReferenceKind(`asset:${id}`), "asset");
});

test("assetToken rejects non-UUID identities", () => {
  assert.throws(() => assetToken("yuki-smile"), /UUID/u);
  assert.equal(isAssetToken("asset:yuki-smile"), false);
});
