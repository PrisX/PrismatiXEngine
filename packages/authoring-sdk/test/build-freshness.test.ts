import assert from "node:assert/strict";
import {mkdtempSync, mkdirSync, rmSync, writeFileSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";

import {assertLocalBuildFresh, writeLocalBuildStamp} from "../src/build-freshness.js";

test("local package source stamp rejects stale dist but skips published packages", t => {
  const root = mkdtempSync(join(tmpdir(), "px-local-build-"));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  mkdirSync(join(root, "src"));
  writeFileSync(join(root, "src", "index.ts"), "export const value = 1;\n");
  writeFileSync(join(root, "package.json"), "{\"name\":\"fixture\"}\n");
  writeFileSync(join(root, "tsconfig.json"), "{}\n");
  assert.throws(() => assertLocalBuildFresh(root, "fixture"), /PXSDKBUILD1001.*npm run build/u);
  writeLocalBuildStamp(root);
  assert.doesNotThrow(() => assertLocalBuildFresh(root, "fixture"));
  writeFileSync(join(root, "src", "index.ts"), "export const value = 2;\n");
  assert.throws(() => assertLocalBuildFresh(root, "fixture"), /PXSDKBUILD1001/u);
  writeLocalBuildStamp(root);
  writeFileSync(join(root, "package.json"), "{\"name\":\"changed\"}\n");
  assert.throws(() => assertLocalBuildFresh(root, "fixture"), /PXSDKBUILD1001/u);
  rmSync(join(root, "src"), {recursive: true});
  assert.doesNotThrow(() => assertLocalBuildFresh(root, "fixture"));
});
