import assert from "node:assert/strict";
import test from "node:test";

import {parseStory, serializeStoryCommand} from "../src/index.js";

test("imported Story command round-trips quoted and structured values", () => {
  const values = {
    text: '她說「a=b ]」 and "yes"',
    asset: "Assets/UI/a b.png",
    payload: {items: [1, true, null], label: "日本語"},
    speed: 1.25,
  };
  const serialized = serializeStoryCommand("stage.compose", values);
  assert.equal(serialized.valid, true, JSON.stringify(serialized.diagnostics));
  const parsed = parseStory(`${serialized.value}\n`, "Story/imported.pxstory");
  assert.equal(parsed.diagnostics.length, 0);
  assert.equal(parsed.nodes[0]?.name, "stage.compose");
  assert.deepEqual(Object.fromEntries(parsed.nodes[0]?.arguments?.map((argument) =>
    [argument.name, argument.value]) ?? []), values);
  const special = JSON.parse('{"payload":{"__proto__":{"name":"kept"}}}') as
    {payload: {"__proto__": {name: string}}};
  const encoded = serializeStoryCommand("stage.compose", special);
  assert.equal(encoded.valid, true);
  assert.deepEqual(parseStory(encoded.value!).nodes[0]?.arguments?.[0]?.value,
    special.payload);
});

test("imported Story command rejects names and values the parser cannot preserve", () => {
  assert.equal(serializeStoryCommand("if", {value: true}).diagnostics[0]?.code,
    "PXIMPORT1002");
  assert.equal(serializeStoryCommand("bad command").valid, false);
  assert.equal(serializeStoryCommand("show", {"bad key": "x"}).diagnostics[0]?.code,
    "PXIMPORT1003");
  assert.equal(serializeStoryCommand("wait", {duration: Number.POSITIVE_INFINITY})
    .diagnostics[0]?.code, "PXIMPORT1004");
  assert.equal(serializeStoryCommand("show", null as never).diagnostics[0]?.code,
    "PXIMPORT1004");
});
