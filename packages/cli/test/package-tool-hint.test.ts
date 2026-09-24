import assert from "node:assert/strict";
import test from "node:test";

import {debugPackagerHint} from "../src/package-tool-hint.js";

test("large artifacts warn only for a selected Debug Packager", () => {
  const debug = "C:\\Engine\\out\\build\\x64-debug\\PrismatiXEngine\\PrismatiXPackager.exe";
  const release = "C:\\Engine\\out\\build\\x64-release\\PrismatiXEngine\\PrismatiXPackager.exe";
  assert.match(debugPackagerHint(debug, 8n * 1024n ** 3n) ?? "", /Release/u);
  assert.equal(debugPackagerHint(debug, 1024n ** 3n - 1n), undefined);
  assert.equal(debugPackagerHint(release, 8n * 1024n ** 3n), undefined);
  assert.equal(debugPackagerHint("/projects/debug-art/Packager", 8n * 1024n ** 3n), undefined);
  assert.match(debugPackagerHint("/engine/out/build/Debug/Packager", 2n * 1024n ** 3n) ?? "", /Debug/u);
});
