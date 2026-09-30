import assert from "node:assert/strict";
import test from "node:test";

import {compileStory, parseStory} from "../src/index.js";

function compile(source: string) {
  return compileStory(parseStory(source, "Story/hide.pxstory"), {documentId: "hide"});
}

test("[hide image=...] compiles to the [image.hide] operation", () => {
  const shape = (source: string) => {
    const result = compile(source);
    assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
    return result.runtimeIr!.operations.map(({kind, arguments: args}) => ({kind, args}));
  };
  assert.deepEqual(shape("[hide image=cg1 400ms]\n[hide image=all]\n[hide image=cg2 duration=1s]\n"),
    shape("[image.hide cg1 400ms]\n[image.hide all]\n[image.hide cg2 duration=1s]\n"));
  assert.deepEqual(shape("[hide yuki]\n[hide all]\n").map(({kind}) => kind),
    ["hideCharacter", "hideCharacters"]);
});

test("[hide image=...] rejects character options", () => {
  const codes = (source: string) => compile(`${source}\n`).diagnostics.map(({code}) => code);
  assert.deepEqual(codes("[hide image=cg1 transition=false]"), ["PXSTORY1245"]);
  assert.deepEqual(codes("[hide character=yuki image=cg1]"), ["PXSTORY1245"]);
  assert.deepEqual(codes("[hide image=cg1 1s 2s]"), ["PXSTORY1245"]);
  assert.deepEqual(codes("[hide yuki duration=1s]"), ["PXSTORY1245"]);
  assert.deepEqual(codes("[hide image=\"a b\"]"), ["PXSTORY1237"]);
  assert.deepEqual(codes("[hide image=cg1 soon]"), ["PXSTORY1236"]);
});
