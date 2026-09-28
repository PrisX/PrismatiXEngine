import assert from "node:assert/strict";
import test from "node:test";

import {createImportedResourceResolver} from "../src/index.js";

test("import resolver requires an unambiguous resource instead of picking by size or order", () => {
  const result = createImportedResourceResolver([
    "Assets/Characters/hero.png", "Assets/UI/hero.png",
    "Assets/Characters/hero.webp", "Assets/BG/school.png",
  ]);
  assert.equal(result.valid, true);
  const resolver = result.value!;
  assert.deepEqual(resolver.resolve("school"),
    {kind: "found", path: "Assets/BG/school.png", match: "name"});
  assert.deepEqual(resolver.resolve("hero.png"), {kind: "ambiguous", query: "hero.png",
    candidates: ["Assets/Characters/hero.png", "Assets/UI/hero.png"]});
  assert.deepEqual(resolver.resolve("hero.webp"),
    {kind: "found", path: "Assets/Characters/hero.webp", match: "name"});
  assert.deepEqual(resolver.resolve("Assets/UI/hero.png"),
    {kind: "found", path: "Assets/UI/hero.png", match: "path"});
  assert.deepEqual(resolver.resolve("Assets/Unknown/hero.png"),
    {kind: "missing", query: "Assets/Unknown/hero.png"});
  assert.deepEqual(resolver.resolve("../hero.png"),
    {kind: "invalid", query: "../hero.png"});
  assert.deepEqual(resolver.resolve("Assets/UI:hero.png"),
    {kind: "invalid", query: "Assets/UI:hero.png"});
});

test("import resolver rejects unsafe inputs and case-colliding exact paths", () => {
  const invalid = createImportedResourceResolver(["Assets/valid.png", "Assets/../outside.png"]);
  assert.equal(invalid.valid, false);
  assert.equal(invalid.diagnostics[0]?.code, "PXIMPORT1001");
  const result = createImportedResourceResolver([
    "Assets/UI/Title.PNG", "assets/ui/title.png", "Assets/UI/Title.PNG",
  ]);
  assert.equal(result.valid, true);
  assert.equal(result.value?.size, 2);
  assert.deepEqual(result.value?.resolve("Assets/UI/Title.PNG"), {
    kind: "ambiguous", query: "Assets/UI/Title.PNG",
    candidates: ["Assets/UI/Title.PNG", "assets/ui/title.png"],
  });
});
