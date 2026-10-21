import assert from "node:assert/strict";
import test from "node:test";

import {extensionBundleEntry, extensionTypeDeclarations, extensionUsesTypeScript, validateDocument} from "../src/index.js";
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
    schemaRevision: 1,
    language: "javascript",
    id: "contract-test",
    version: "1.0.0",
    requiredEngineVersion: "^0.1.0",
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

test("extension Action manifest rejects contracts the native catalog cannot register", () => {
  const invalid = validateDocument<ExtensionManifest>("extension", manifest({
    safety,
    actions: [{id: "bad.action", capabilities: ["audio"], parameters: [
      {name: "nothing", type: "null"},
      {name: "volume", type: "number", default: "loud"},
      {name: "mode", type: "integer", enum: [1, 2], editorHint: "enum"},
      {name: "media", type: "string", resourceFilter: "audio"},
    ]}],
  }), "Extensions/bad.pxextension");
  assert.equal(invalid.valid, false);
  for (const code of ["PXSDKSEM1114", "PXSDKSEM1115", "PXSDKSEM1116",
    "PXSDKSEM1117", "PXSDKSEM1118"])
    assert.ok(invalid.diagnostics.some((diagnostic) => diagnostic.code === code), code);
});

test("extension entry and modules may be TypeScript, not declarations or TSX", () => {
  const typescript = validateDocument<ExtensionManifest>("extension", manifest({
    language: "typescript", entry: "main.ts", modules: ["lib/util.ts", "lib/legacy.js"],
  }), "Extensions/test.pxextension");
  assert.equal(typescript.valid, true, JSON.stringify(typescript.diagnostics));
  const declaration = validateDocument<ExtensionManifest>("extension", manifest({entry: "types.d.ts"}),
    "Extensions/test.pxextension");
  assert.ok(declaration.diagnostics.some((diagnostic) => diagnostic.code === "PXSDKSEM1124"));
  const tsx = validateDocument<ExtensionManifest>("extension", manifest({entry: "main.tsx"}),
    "Extensions/test.pxextension");
  assert.equal(tsx.valid, false);
  // A JavaScript entry runs unbundled, so it cannot import TypeScript.
  const mixed = validateDocument<ExtensionManifest>("extension", manifest({modules: ["lib/util.ts"]}),
    "Extensions/test.pxextension");
  assert.ok(mixed.diagnostics.some((diagnostic) => diagnostic.code === "PXSDKSEM1125"));
});

test("extension type declarations follow the manifest", () => {
  const text = extensionTypeDeclarations(manifest({
    id: "typed", capabilities: ["runtime", "ui"], safety,
    commands: [{id: "game.raise", await: true, rollback: "reversible", displayName: "Raise */ it",
      parameters: [
        {name: "amount", type: "integer", required: true, range: {minimum: 0, maximum: 9}},
        {name: "mood", type: "string", enum: ["calm", "angry"], default: "calm"},
        {name: "note", type: "string"},
        {name: "art", type: "resource"},
      ]}],
    actions: [{id: "game.open", reentry: "ignoreWhileRunning", parameters: [
      {name: "at", type: "vec2", required: true}, {name: "tint", type: "color"}]}],
  }), "Content/Extensions/typed/typed.pxextension");
  assert.match(text, /import type \{Color, Rect, ResourceValue, RuntimeValue, Vec2Value\} from "@prismatix\/runtime";/u);
  assert.match(text, /readonly id: "typed";/u);
  assert.match(text, /readonly capabilities: "runtime" \| "ui";/u);
  assert.match(text, /"game\.raise": \{\n {6}readonly args: \{\n/u);
  assert.match(text, /readonly "amount": number;/u);
  assert.match(text, /readonly "mood": "calm" \| "angry";/u);
  assert.match(text, /readonly "note"\?: string;/u);
  assert.match(text, /readonly "art"\?: ResourceValue;/u);
  assert.match(text, /readonly await: true;\n {6}readonly rollback: "reversible";/u);
  assert.match(text, /readonly "at": Vec2Value;/u);
  assert.match(text, /readonly "tint"\?: Color;/u);
  assert.match(text, /readonly reentry: "ignoreWhileRunning";/u);
  assert.ok(text.includes(String.raw`Raise *\/ it`), "a description cannot end the comment");
  assert.equal(text, extensionTypeDeclarations(manifest({
    id: "typed", capabilities: ["runtime", "ui"], safety,
    commands: [{id: "game.raise", await: true, rollback: "reversible", displayName: "Raise */ it",
      parameters: [
        {name: "amount", type: "integer", required: true, range: {minimum: 0, maximum: 9}},
        {name: "mood", type: "string", enum: ["calm", "angry"], default: "calm"},
        {name: "note", type: "string"},
        {name: "art", type: "resource"},
      ]}],
    actions: [{id: "game.open", reentry: "ignoreWhileRunning", parameters: [
      {name: "at", type: "vec2", required: true}, {name: "tint", type: "color"}]}],
  }), "Content/Extensions/typed/typed.pxextension"), "generation is deterministic");
  assert.equal(extensionBundleEntry("src/main.ts"), "src/main.js");
  assert.equal(extensionUsesTypeScript({entry: "main.js", modules: ["a.ts"]}), true);
  assert.equal(extensionUsesTypeScript({entry: "main.js"}), false);
});
