import assert from "node:assert/strict";
import test from "node:test";

import {compileStory, compileStoryProject, parseStory, stableSourceIdReport} from "../src/index.js";
import type {GameDocument} from "../src/index.js";

const game: GameDocument = {
  format: "PrismatiXGame", schemaRevision: 2,
  variables: [{name: "affection", type: "integer", default: 0, scope: "session"}],
  gallery: [], unlockables: [],
};

function compile(source: string) {
  return compileStory(parseStory(source, "Story/flow.pxstory"), {documentId: "flow", game});
}

function operations(source: string) {
  const result = compile(source);
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  return result.runtimeIr!.operations;
}

function codes(source: string) {
  return compile(source).diagnostics.map(({code}) => code);
}

test("[elseif] lowers to else and a nested condition closed by one [endif]", () => {
  const ops = operations([
    "[if affection > 5]", "high", "[elseif affection > 2]", "middle",
    "[elseif affection > 0]", "low", "[else]", "none", "[endif]", "after", "",
  ].join("\n"));
  assert.deepEqual(ops.map(({kind}) => kind), [
    "condition", "narration", "else", "condition", "narration", "else", "condition",
    "narration", "else", "narration", "endCondition", "endCondition", "endCondition",
    "narration",
  ]);
  const elseif = ops[2]!;
  assert.equal(ops[3]!.sourceId, `${elseif.sourceId}/elseif`);
  const endif = ops[10]!;
  assert.deepEqual(ops.slice(11, 13).map(({sourceId}) => sourceId),
    [`${endif.sourceId}/elseif1`, `${endif.sourceId}/elseif2`]);
  assert.equal(new Set(ops.map(({operationId}) => operationId)).size, ops.length);
});

test("[if] without [elseif] compiles exactly as before", () => {
  const ops = operations("[if affection > 0]\nyes\n[else]\nno\n[endif]\n");
  assert.deepEqual(ops.map(({kind}) => kind),
    ["condition", "narration", "else", "narration", "endCondition"]);
  const nodes = parseStory("[if affection > 0]\nyes\n[else]\nno\n[endif]\n", "Story/flow.pxstory").nodes;
  assert.deepEqual(ops.map(({sourceId}) => sourceId), nodes.map(({id}) => id));
});

test("nested [if] inside an [elseif] branch closes the right chains", () => {
  const ops = operations([
    "[if affection > 5]", "a", "[elseif affection > 2]", "[if affection > 3]", "b", "[endif]",
    "[endif]", "",
  ].join("\n"));
  assert.deepEqual(ops.map(({kind}) => kind), [
    "condition", "narration", "else", "condition", "condition", "narration",
    "endCondition", "endCondition", "endCondition",
  ]);
});

test("[elseif] misuse is reported", () => {
  assert.deepEqual(codes("[elseif affection > 0]\nx\n"), ["PXSTORY1243"]);
  assert.deepEqual(codes("[if affection > 0]\n[else]\n[elseif affection > 1]\n[endif]\n"),
    ["PXSTORY1244"]);
  assert.deepEqual(codes("[if affection > 0]\n[elseif affection >]\n[endif]\n"), ["PXSTORY1133"]);
  assert.deepEqual(codes("[if affection > 0]\n[elseif unknown > 1]\n[endif]\n"), ["PXSTORY1133"]);
});

test("conditional choices carry their encoded condition", () => {
  const ops = operations([
    "[choice text=\"告白する\" goto=confess if=\"affection > 3\"]",
    "[choice text=\"散歩する\" goto=walk]", "[choice.wait]",
    "*confess", "a", "*walk", "b", "",
  ].join("\n"));
  const [confess, walk] = ops.filter(({kind}) => kind === "choiceOption");
  assert.deepEqual(JSON.parse(confess!.arguments.condition!), {kind: "binary", op: "greater",
    left: {kind: "variable", name: "affection"}, right: {kind: "literal", value: 3}});
  assert.equal(walk!.arguments.condition, undefined);
  assert.deepEqual(codes("[choice text=x goto=a if=\"affection >\"]\n*a\n"), ["PXSTORY1133"]);
  assert.deepEqual(codes("[choice text=x goto=a if=\"nobody > 1\"]\n*a\n"), ["PXSTORY1133"]);
});

test("a [choice] block compiles to the same operations as [choice] lines", () => {
  const block = operations([
    "[choice]",
    "- 一起看看星星 -> stars",
    "; a comment is fine",
    "",
    "- 告白する -> confess if affection > 3",
    "- \"A -> B\" -> story",
    "[endchoice]",
    "*stars", "x", "*confess", "y", "*story", "z", "",
  ].join("\n"));
  const lines = operations([
    "[choice text=\"一起看看星星\" goto=stars]",
    "[choice text=\"告白する\" goto=confess if=\"affection > 3\"]",
    "[choice text=\"A -> B\" goto=story]",
    "*stars", "x", "*confess", "y", "*story", "z", "",
  ].join("\n"));
  const shape = (ops: typeof block) => ops.map(({kind, text, arguments: args}) => ({kind, text, args}));
  assert.deepEqual(shape(block), shape(lines));
  // Each option is its own observable operation for identity sidecars.
  const report = stableSourceIdReport("[choice]\n- a -> x\n- b -> y\n[endchoice]\n*x\n*y\n");
  assert.deepEqual(report.entries.map(({command, line}) => [command, line]),
    [["choice", 2], ["choice", 3], [undefined, 5], [undefined, 6]]);
});

test("[choice] block mistakes are reported", () => {
  // Without [endchoice] the block runs to the end, so the label is lost too.
  assert.deepEqual(codes("[choice]\n- a -> x\n*x\n"), ["PXSTORY1240", "PXSTORY1241", "PXSTORY1117"]);
  assert.deepEqual(codes("[choice]\n- a -> x\nprose\n[endchoice]\n*x\n"), ["PXSTORY1240"]);
  assert.deepEqual(codes("[choice]\n- a\n[endchoice]\n"), ["PXSTORY1240", "PXSTORY1242"]);
  assert.deepEqual(codes("[choice]\n- -> x\n[endchoice]\n*x\n"), ["PXSTORY1240", "PXSTORY1242"]);
  assert.deepEqual(codes("[endchoice]\n"), ["PXSTORY1241"]);
  assert.deepEqual(codes("[choice]\n- a -> missing\n[endchoice]\n"), ["PXSTORY1117"]);
  // A bare [choice] not followed by options keeps its old diagnostic.
  assert.deepEqual(codes("[choice]\nprose\n"), ["PXSTORY1116"]);
});

test("block options reach other scenes with scene=", () => {
  const index = {format: "PrismatiXStoryIndex" as const, schemaRevision: 2 as const, id: "flow",
    entryScene: "a", chapters: [{id: "c", title: "C", scenes: ["a", "b"]}],
    scenes: [{id: "a", sources: {en: "Story/a.pxstory"}}, {id: "b", sources: {en: "Story/b.pxstory"}}]};
  const result = compileStoryProject({storyIndex: index, locale: "en", game, sourceFiles: {
    "Story/a.pxstory": "[choice]\n- next -> scene=b\n- there -> there scene=b if affection > 0\n[endchoice]\n",
    "Story/b.pxstory": "b\n*there\nthere\n",
  }});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const options = result.runtimeIr!.operations.filter(({kind}) => kind === "choiceOption");
  assert.equal(options.length, 2);
  assert.ok(options[0]!.arguments.target!.startsWith("pxscene.entry."));
  assert.ok(options[1]!.arguments.target!.startsWith("pxscene.label."));
  assert.ok(options[1]!.arguments.condition !== undefined);
});
