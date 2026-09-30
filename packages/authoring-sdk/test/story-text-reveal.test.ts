import assert from "node:assert/strict";
import test from "node:test";

import {compileStory, parseStory} from "../src/index.js";

function compile(source: string) {
  return compileStory(parseStory(source, "Story/reveal.pxstory"), {documentId: "reveal"});
}

test("reveal tags stay in the dialogue text and pass validation", () => {
  const result = compile([
    "@直哉「待って[w 500ms]……[speed 50%]ゆっくり[/speed]、[nowait]一気に！[/nowait]」",
    "[w 1s]間を置いてから話す。",
    "[nowait]すぐ出る行。[/nowait]",
    "[speed 200%]早口[/speed][w 0.5s]",
    "",
  ].join("\n"));
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.diagnostics, []);
  const ops = result.runtimeIr!.operations;
  assert.deepEqual(ops.map(({kind}) => kind), ["dialogue", "narration", "narration", "narration"]);
  assert.equal(ops[0]!.arguments.text,
    "「待って[w 500ms]……[speed 50%]ゆっくり[/speed]、[nowait]一気に！[/nowait]」");
  assert.equal(ops[1]!.arguments.text, "[w 1s]間を置いてから話す。");
});

test("malformed or unbalanced reveal tags are reported", () => {
  const codes = (source: string) => compile(`${source}\n`).diagnostics.map(({code, severity}) => `${code}:${severity}`);
  assert.deepEqual(codes("a[w soon]b"), ["PXSTORY1185:error"]);
  assert.deepEqual(codes("a[w 61s]b"), ["PXSTORY1185:error"]);
  assert.deepEqual(codes("a[speed 5%]b[/speed]"), ["PXSTORY1185:error", "PXSTORY1182:warning"]);
  assert.deepEqual(codes("a[speed fast]b"), ["PXSTORY1185:error"]);
  assert.deepEqual(codes("a[speed 50%]b"), ["PXSTORY1182:warning"]);
  assert.deepEqual(codes("a[/nowait]b"), ["PXSTORY1182:warning"]);
  assert.deepEqual(codes("[ruby=よみ]読[w 1s]み[/ruby]"), ["PXSTORY1184:warning"]);
});

test("reveal tag names are reserved for text, not macros", () => {
  const result = compile("[macro w]\nx\n[endmacro]\n");
  assert.deepEqual(result.diagnostics.map(({code}) => code), ["PXSTORY1191"]);
});
