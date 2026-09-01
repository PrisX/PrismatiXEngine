import assert from "node:assert/strict";
import test from "node:test";

import {validateDocument} from "../src/index.js";
import type {ExtensionManifest} from "../src/index.js";

const safety = {
  previewSafe: true,
  deterministic: true,
  seekSafe: true,
  rollbackSafe: true,
} as const;

function manifest(overrides: Partial<ExtensionManifest> = {}): ExtensionManifest {
  return {
    format: "PrismatiXExtension",
    schemaRevision: 2,
    language: "javascript",
    id: "contract-test",
    version: "1.0.0",
    requiredEngineVersion: "^0.2.0",
    entry: "extension.js",
    capabilities: ["runtime"],
    commands: [],
    actions: [],
    ...overrides,
  };
}

test("extension declarations may inherit a manifest safety contract", () => {
  const value = manifest({
    safety,
    commands: [{
      id: "safe-command",
      rollback: "reversible",
      parameters: [],
    }],
    actions: [{
      id: "safe-action",
      parameters: [],
    }],
  });

  const result = validateDocument<ExtensionManifest>("extension", value, "Extensions/test.pxextension");
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
});

test("extension declarations without local or inherited safety fail authoring validation", () => {
  const value = manifest({
    commands: [{
      id: "unsafe-command",
      rollback: "boundary",
      parameters: [],
    }],
  });

  const result = validateDocument<ExtensionManifest>("extension", value, "Extensions/test.pxextension");
  assert.equal(result.valid, false);
  assert.ok(result.diagnostics.some((diagnostic) => diagnostic.code === "PXSDKSEM1109"));
});

test("declaration-level safety remains valid without manifest-level safety", () => {
  const value = manifest({
    commands: [{
      id: "locally-safe-command",
      rollback: "transient",
      safety,
      parameters: [],
    }],
  });

  const result = validateDocument<ExtensionManifest>("extension", value, "Extensions/test.pxextension");
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
});

test("extension ES modules are explicit, path-safe JavaScript inputs", () => {
  const valid = validateDocument<ExtensionManifest>("extension", manifest({
    modules: ["lib/messages.js", "lib/runtime.js"],
  }), "Extensions/test.pxextension");
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));

  const invalid = validateDocument<ExtensionManifest>("extension", {
    ...manifest(),
    modules: ["../foreign.js"],
  }, "Extensions/test.pxextension");
  assert.equal(invalid.valid, false);
});
