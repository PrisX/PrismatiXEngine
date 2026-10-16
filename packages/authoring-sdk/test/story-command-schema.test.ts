import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";

import {compileStory, parseStory, storyActorActions, storyAssetCategories, storyCommandCompletions,
  storyCommandFamilies, storyCommandSchema, storyEaseNames} from "../src/index.js";
import type {ExtensionCommand, StoryCommandParameter, StoryCommandSchema} from "../src/index.js";

const schema = storyCommandSchema();
const byName = new Map(schema.map((command) => [command.name, command]));
const command = (name: string): StoryCommandSchema => byName.get(name)!;
const parameter = (name: string, parameterName: string): StoryCommandParameter =>
  command(name).parameters.find((item) => item.name === parameterName)!;

function compile(source: string) {
  return compileStory(parseStory(`${source}\n`, "Story/schema.pxstory"), {documentId: "schema"});
}

test("the schema describes every built-in command compileStory knows, once", () => {
  // Drift guard: the command lists in story.ts are the compiler's truth.
  const source = readFileSync(new URL("../../src/story.ts", import.meta.url), "utf8");
  const listed = /const builtinStoryCommands = \[([^\]]*)\]/u.exec(source)![1]!;
  const builtins = new Set([...listed.matchAll(/"([^"]+)"/gu)].map((match) => match[1]!));
  const named = /const builtinNamedArguments[^=]*= \{([\s\S]*?)\n\};/u.exec(source)![1]!;
  for (const match of named.matchAll(/(?:^|[\s,{])("?)([A-Za-z.]+)\1:\s*\[/gu)) builtins.add(match[2]!);
  assert.deepEqual(new Set(schema.map((item) => item.name)), builtins);
  assert.equal(byName.size, schema.length, "duplicate command names");
  for (const item of schema) {
    assert.ok(storyCommandFamilies.includes(item.family), item.name);
    assert.ok(item.description.length > 0, item.name);
    const names = item.parameters.map((value) => value.name);
    assert.equal(new Set(names).size, names.length, `${item.name} repeats a parameter`);
  }
  assert.equal(command("choice").block, "choice");
  assert.equal(command("macro").block, "macro");
  assert.equal(command("credits").block, "credits");
  assert.equal(command("endchoice").closes, "choice");
  assert.equal(command("endmacro").closes, "macro");
  assert.equal(command("endcredits").closes, "credits");
  // The schema is plain JSON for editors and bridges.
  assert.deepEqual(JSON.parse(JSON.stringify(schema)), schema);
});

test("every named parameter and alias is one compileStory accepts", () => {
  for (const item of schema) {
    if (item.expression === true || item.assignment === true || item.variadic === true) continue;
    for (const name of item.parameters.flatMap((value) => [value.name, ...value.aliases ?? []])) {
      const result = compile(`[${item.name} ${name}=x]`);
      assert.ok(!result.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1017"),
        `[${item.name} ${name}=...] is rejected as unknown`);
    }
  }
});

test("value kinds use the engine's names", () => {
  // Engine/Support/Easing.h maps these names in Ease(); unknown names are linear.
  const easing = readFileSync(new URL("../../../../PrismatiXEngine/Engine/Support/Easing.h",
    import.meta.url), "utf8");
  const native = [...easing.slice(easing.indexOf("inline float Ease(")).matchAll(/name == "([A-Za-z]+)"\) return/gu)]
    .map((match) => match[1]);
  assert.deepEqual([...storyEaseNames], native);
  assert.deepEqual(parameter("act", "preset").kind, {type: "enum", values: [...storyActorActions]});
  for (const item of schema) for (const value of item.parameters) {
    if (value.kind.type === "asset") assert.ok(storyAssetCategories.includes(value.kind.category));
    if (value.kind.type === "ease") assert.ok(storyEaseNames.every((name) => (value.kind as {values: readonly string[]}).values.includes(name)));
  }
});

// Commands whose bounded parameters compileStory validates, with a line that
// is valid without them; `{}` is replaced by `name=value`.
const bounded: Readonly<Record<string, {readonly line: string; readonly parameters: readonly string[]}>> = {
  credits: {line: "[credits {}]\nName\n[endcredits]", parameters: ["duration", "size"]},
  transition: {line: "[transition {}]", parameters: ["duration", "vague"]},
  se: {line: "[se Assets/se/a.ogg {}]", parameters: ["channel", "volume"]},
  "se.stop": {line: "[se.stop {}]", parameters: ["fade", "channel"]},
  bgm: {line: "[bgm Assets/bgm/a.ogg {}]", parameters: ["fade", "volume"]},
  voice: {line: "[voice Assets/voice/a.ogg {}]", parameters: ["volume"]},
  input: {line: "[input var=name {}]", parameters: ["max"]},
  flash: {line: "[flash {}]", parameters: ["alpha", "duration", "loop"]},
  shake: {line: "[shake 300ms dir=h {}]", parameters: ["count"]},
  auto: {line: "[auto on {}]", parameters: ["speed", "wait"]},
  hud: {line: "[hud false {}]", parameters: ["time"]},
  act: {line: "[act kei nod {}]", parameters: ["size", "time", "loop"]},
  image: {line: "[image cg Assets/bg/a.png {}]",
    parameters: ["z", "alpha", "from-alpha", "zoom", "from-zoom", "duration"]},
  "image.hide": {line: "[image.hide cg {}]", parameters: ["duration"]},
  bg: {line: "[bg Assets/bg/a.png {}]", parameters: ["zoom", "from-zoom"]},
  motion: {line: "[motion Assets/a.pxmotion {}]", parameters: ["speed"]},
  timeline: {line: "[timeline Assets/a.pxtimeline {}]", parameters: ["speed"]},
  camera: {line: "[camera x=10 {}]", parameters: ["zoom", "duration"]},
  add: {line: "[add name=score {}]", parameters: ["amount"]},
};

test("ranges match compileStory at and just past every bound", () => {
  let checked = 0;
  for (const [name, {line, parameters}] of Object.entries(bounded)) {
    for (const parameterName of parameters) {
      const {kind} = parameter(name, parameterName);
      const accepts = (value: string) => compile(line.replace("{}", `${parameterName}=${value}`)).valid;
      const expect = (value: string, valid: boolean) => {
        assert.equal(accepts(value), valid, `[${name} ${parameterName}=${value}] should be ${valid ? "valid" : "invalid"}`);
        checked += 1;
      };
      if (kind.type === "duration") {
        expect(`${kind.maxMs}ms`, true);
        expect(`${kind.maxMs + 1}ms`, false);
        if (kind.minMs > 0) {
          expect(`${kind.minMs}ms`, true);
          expect(`${kind.minMs - 1}ms`, false);
        }
      } else if (kind.type === "integer") {
        assert.ok(kind.min !== undefined && kind.max !== undefined, `${name} ${parameterName} has no range`);
        expect(String(kind.min), true);
        expect(String(kind.max), true);
        expect(String(kind.min - 1), false);
        expect(String(kind.max + 1), false);
        for (const excluded of kind.excluded ?? []) expect(String(excluded), false);
      } else if (kind.type === "number") {
        assert.ok(kind.min !== undefined, `${name} ${parameterName} has no minimum`);
        if (kind.exclusiveMin === true) {
          expect(String(kind.min), false);
          expect(String(kind.min + 0.5), true);
        } else {
          expect(String(kind.min), true);
          expect(String(kind.min - 1), false);
        }
        if (kind.max !== undefined) {
          expect(String(kind.max), true);
          expect(String(kind.max + 1), false);
        }
      } else assert.fail(`${name} ${parameterName} is not a bounded kind`);
    }
  }
  assert.ok(checked > 60);
});

test("positionals follow compileStory's short forms", () => {
  const operation = (source: string) => {
    const result = compile(source);
    assert.equal(result.valid, true, `${source}: ${JSON.stringify(result.diagnostics)}`);
    return result.runtimeIr!.operations.at(-1)!;
  };
  // show: alias first, then expression and position in either order.
  assert.deepEqual(command("show").anyOrder, ["expression", "position"]);
  assert.equal(operation("[show kei left smile]").arguments.position, "1");
  assert.equal(operation("[show kei smile right]").arguments.position, "3");
  assert.equal(operation("[show character=kei right]").arguments.position, "3");
  // act shifts: without the alias the first unnamed word is the preset.
  assert.equal(command("act").positionals, "shift");
  assert.equal(parameter("act", "preset").positional, 1);
  assert.equal(operation("[image cg Assets/bg/a.png]\n[act image=cg float]").arguments.preset, "float");
  assert.equal(operation("[act character=kei hop]").arguments.preset, "hop");
  // hide image= takes its duration as the first unnamed word.
  assert.deepEqual(parameter("hide", "duration").requires, ["image"]);
  assert.equal(operation("[hide image=cg 400ms]").arguments.duration, "400ms");
  // image.hide is fixed: the duration is always the second unnamed word.
  assert.equal(command("image.hide").positionals, "fixed");
  assert.equal(operation("[image.hide cg 300ms]").arguments.duration, "300ms");
  // chapter takes every word; if is one expression.
  assert.equal(parameter("chapter", "title").rest, true);
  assert.equal(operation("[chapter 第一章 天台]").arguments.title, "第一章 天台");
  assert.equal(command("if").expression, true);
  // jump/call/choice read target=, goto= or the first word.
  assert.deepEqual(parameter("jump", "target").aliases, ["goto"]);
  assert.equal(operation("*a\n[jump goto=a]").arguments.target, "a");
  // Defaults the compiler fills in itself.
  assert.equal(parameter("transition", "duration").default, "500ms");
  assert.equal(operation("[transition]").arguments.duration, "500ms");
  assert.equal(parameter("ui", "operation").default, operation("[ui save]").arguments.operation);
  const flash = operation("[flash]").arguments;
  for (const name of ["alpha", "duration", "loop", "color"])
    assert.equal(parameter("flash", name).default, flash[name]);
});

test("extension commands join the schema and completions stay derived from it", () => {
  const extension: ExtensionCommand = {
    id: "studio.weather",
    description: "Sets the weather",
    parameters: [
      {name: "kind", type: "string", required: true, enum: ["rain", "snow"]},
      {name: "strength", type: "number", range: {minimum: 0, maximum: 1}, default: 0.5},
      {name: "sprite", type: "resource", resourceFilter: "*.png"},
      {name: "area", type: "rect"},
    ],
  };
  const described = storyCommandSchema([extension]).at(-1)!;
  assert.equal(described.extension, true);
  assert.deepEqual(described.parameters, [
    {name: "kind", kind: {type: "enum", values: ["rain", "snow"]}, required: true},
    {name: "strength", kind: {type: "number", min: 0, max: 1}, default: "0.5"},
    {name: "sprite", kind: {type: "resource", extensions: [], filter: "*.png"}},
    {name: "area", kind: {type: "json"}},
  ]);
  assert.ok(described.parameters.every((value) => value.positional === undefined));
  const completions = storyCommandCompletions([extension]);
  assert.equal(completions.length, schema.length + 1);
  assert.deepEqual(completions.at(-1)!.parameters[0], {name: "kind", required: true, values: ["rain", "snow"]});
  assert.deepEqual(completions.find((item) => item.id === "move")!.parameters
    .find((item) => item.name === "ease")!.values, [...storyEaseNames]);
  assert.deepEqual(completions.find((item) => item.id === "bgm")!.parameters
    .find((item) => item.name === "loop"), {name: "loop", required: false, values: [true, false]});
});
