import assert from "node:assert/strict";
import test from "node:test";

import {compileStory, parseStory} from "../src/index.js";
import type {GameDocument} from "../src/index.js";

const game: GameDocument = {
  format: "PrismatiXGame", schemaRevision: 2,
  variables: [
    {name: "hero", type: "string", default: "直哉", scope: "profile"},
    {name: "route", type: "integer", default: 0, scope: "session"},
  ],
  gallery: [], unlockables: [],
};

function compile(source: string, withGame = true) {
  return compileStory(parseStory(source, "Story/input.pxstory"),
    {documentId: "input", ...(withGame ? {game} : {})});
}

test("[input] asks for text and stores it in a string variable", () => {
  const result = compile("[input var=hero prompt=\"名前を入力\" default=直哉 max=8]\nこんにちは。\n");
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const input = result.runtimeIr!.operations[0]!;
  assert.equal(input.kind, "ui");
  assert.deepEqual(input.arguments, {route: "input", operation: "input", variable: "hero",
    prompt: "名前を入力", default: "直哉", maxLength: "8", scope: "profile"});
  // A [var] declared in the Story works too, and max defaults to 32.
  const declared = compile("[var nickname=\"\" type=string]\n[input nickname]\n", false);
  assert.equal(declared.valid, true, JSON.stringify(declared.diagnostics));
  assert.equal(declared.runtimeIr!.operations.at(-1)!.arguments.maxLength, "32");
});

test("[input] rejects a missing, unknown or non-text variable and a bad max", () => {
  const codes = (source: string) => compile(source).diagnostics.map(({code}) => code);
  assert.deepEqual(codes("[input]\n"), ["PXSTORY1420"]);
  assert.deepEqual(codes("[input var=nobody]\n"), ["PXSTORY1122"]);
  assert.deepEqual(codes("[input var=route]\n"), ["PXSTORY1123"]);
  assert.deepEqual(codes("[input var=hero max=0]\n"), ["PXSTORY1421"]);
  assert.deepEqual(codes("[input var=hero max=999]\n"), ["PXSTORY1421"]);
});
