import assert from "node:assert/strict";
import test from "node:test";

import {compileProject, compileStory, externalizeStoryIds, parseStory} from "../src/index.js";
import type {GameDocument} from "../src/index.js";

const game: GameDocument = {
  format: "PrismatiXGame", schemaRevision: 1,
  variables: [{name: "route", type: "string", default: "common", scope: "session"}],
  gallery: [], unlockables: [],
};

function compile(source: string, withGame = true) {
  return compileStory(parseStory(source, "Story/vars.pxstory"),
    {documentId: "vars", ...(withGame ? {game} : {})});
}

test("[var] declares a typed variable that [set], [add] and [if] can use", () => {
  const result = compile([
    "[var affection=0]", "[var met_aoi=false scope=profile]", "[var ratio=0.5]",
    "[var nickname=naoya type=string]", "[var total=0 type=number]",
    "[add affection 2]", "[set met_aoi=true]", "[set ratio=1.5]", "[set nickname=\"直哉\"]",
    "[if affection > 1]", "yes", "[endif]", "",
  ].join("\n"));
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const ops = result.runtimeIr!.operations;
  assert.deepEqual(ops.map(({kind}) => kind),
    ["setVariable", "setVariable", "setVariable", "setVariable", "condition", "narration", "endCondition"]);
  assert.deepEqual(ops[1]!.arguments, {name: "met_aoi", value: "true", scope: "profile", type: "boolean"});
  assert.deepEqual(ops[0]!.arguments, {name: "affection", add: "2", scope: "session"});
  // Types are checked as for .pxgame variables, with or without a catalog.
  assert.deepEqual(compile("[var affection=0]\n[set affection=true]\n").diagnostics.map(({code}) => code),
    ["PXSTORY1123"]);
  assert.deepEqual(compile("[var affection=0]\n[set affection=true]\n", false).diagnostics
    .map(({code}) => code), ["PXSTORY1123"]);
});

test("malformed and conflicting declarations are errors", () => {
  const codes = (source: string) => compile(source).diagnostics.map(({code}) => code);
  for (const source of ["[var]", "[var affection]", "[var a=1 b=2]", "[var a=1 scope=world]",
    "[var a=1 type=list]", "[var a=1.5 type=integer]", "[var a=yes type=boolean]",
    "[var a=[1] ]", "[var a=null]"])
    assert.deepEqual(codes(`${source}\n`), ["PXSTORY1250"], source);
  assert.deepEqual(codes("[var route=\"common\"]\n[var route=common]\n"), [],
    "an identical redeclaration is allowed");
  assert.deepEqual(codes("[var route=1]\n"), ["PXSTORY1251"]);
  assert.deepEqual(codes("[var a=1]\n[var a=2]\n"), ["PXSTORY1251"]);
  assert.deepEqual(codes("[var a=1]\n[var a=1 scope=profile]\n"), ["PXSTORY1251"]);
  assert.deepEqual(codes("[macro m]\n[var a=1]\n[endmacro]\n[m]\n"), ["PXSTORY1252"]);
});

test("[var] emits no operation and takes no identity sidecar entry", () => {
  const externalized = externalizeStoryIds("[var affection=0]\nこんにちは。\n");
  assert.equal(externalized.valid, true, JSON.stringify(externalized.diagnostics));
  assert.deepEqual(externalized.map!.entries.map(({kind}) => kind), ["narration"]);
});

test("project builds merge Story declarations into the published game catalog", () => {
  const compiled = compileProject({project: {name: "Vars"}, sourceFiles: {
    "Story/main.pxstory": "[var affection=0]\n[var seen_end=false scope=profile]\n[add affection 1]\nこんにちは。\n",
  }});
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  const catalog = JSON.parse(compiled.artifact!.files["Content/game.pxgame"]!) as GameDocument;
  assert.deepEqual(catalog.variables, [
    {name: "affection", type: "integer", default: 0, scope: "session"},
    {name: "seen_end", type: "boolean", default: false, scope: "profile"},
  ]);

  // Declarations in several scenes and an explicit .pxgame agree or fail.
  const index = {format: "PrismatiXStoryIndex", schemaRevision: 1, id: "main", entryScene: "a",
    chapters: [{id: "c", title: "C", scenes: ["a", "b"]}],
    scenes: [{id: "a", sources: {"en-US": "Story/a.pxstory"}}, {id: "b", sources: {"en-US": "Story/b.pxstory"}}]};
  const project = {name: "Vars", storyIndex: "Story/story.pxindex", gameCatalog: "Content/game.pxgame"};
  const documents = {"Story/story.pxindex": index, "Content/game.pxgame": game};
  const agreeing = compileProject({project, documents, sourceFiles: {
    "Story/a.pxstory": "[var affection=0]\n[set route=\"a\"]\n[jump scene=b]\n",
    "Story/b.pxstory": "[var affection=0]\n[add affection 1]\n",
  }});
  assert.equal(agreeing.valid, true, JSON.stringify(agreeing.diagnostics));
  const merged = JSON.parse(agreeing.artifact!.files["Content/game.pxgame"]!) as GameDocument;
  assert.deepEqual(merged.variables.map(({name}) => name), ["route", "affection"]);
  const conflicting = compileProject({project, documents, sourceFiles: {
    "Story/a.pxstory": "[var affection=0]\n[var route=1]\n",
    "Story/b.pxstory": "[var affection=1]\n",
  }});
  const issues = conflicting.diagnostics.filter(({code}) => code === "PXSTORY1251");
  assert.deepEqual(issues.map(({path}) => path), ["Story/a.pxstory", "Story/b.pxstory"]);
  assert.match(issues[1]!.message, /Story\/a\.pxstory:1/u);
  // Without declarations the authored catalog is published unchanged.
  const plain = compileProject({project, documents, sourceFiles: {
    "Story/a.pxstory": "a\n", "Story/b.pxstory": "b\n"}});
  assert.equal(plain.valid, true, JSON.stringify(plain.diagnostics));
  assert.deepEqual(JSON.parse(plain.artifact!.files["Content/game.pxgame"]!), game);
});
