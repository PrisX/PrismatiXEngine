import assert from "node:assert/strict";
import {test} from "node:test";

import {completionContext, targetAt} from "../src/server/analysis.js";
import {bundledSdk} from "../src/server/sdk.js";

const context = (line: string, character = line.length) => completionContext(bundledSdk, line, character);

test("completion context: command names and arguments", () => {
  assert.deepEqual(context("  [bgm.st"), {kind: "commandName", prefix: "bgm.st", columns: [3, 9]});
  const value = context("[bg street rule=wi");
  assert.equal(value.kind, "argument");
  assert.ok(value.kind === "argument");
  assert.equal(value.command, "bg");
  assert.equal(value.name, "rule");
  assert.equal(value.prefix, "wi");
  assert.deepEqual(value.columns, [16, 18]);
  const fresh = context("[show guide ");
  assert.ok(fresh.kind === "argument");
  assert.equal(fresh.name, undefined);
  assert.equal(fresh.positionalIndex, 1);
  assert.deepEqual(fresh.columns, [12, 12]);
  const quoted = context("[choice text=\"公園");
  assert.ok(quoted.kind === "argument");
  assert.equal(quoted.name, "text");
  assert.equal(quoted.prefix, "公園");
  assert.equal(quoted.quote, "\"");
  assert.deepEqual(context("[bg street]"), {kind: "none"});
});

test("completion context: speakers, options and inline tags", () => {
  assert.deepEqual(context("@直"), {kind: "speakerName", prefix: "直", columns: [1, 2]});
  const option = context("  @guide[smile voice=g0");
  assert.ok(option.kind === "speakerOption");
  assert.equal(option.speaker, "guide");
  assert.equal(option.name, "voice");
  assert.equal(option.prefix, "g0");
  assert.deepEqual(option.columns, [21, 23]);
  assert.deepEqual(context("@guide 文字[ru"), {kind: "inlineTag", prefix: "ru", columns: [10, 12]});
  assert.deepEqual(context("[b]Loud[/b] and [size="), {kind: "inlineTagValue", tag: "size", prefix: "",
    columns: [22, 22]});
  assert.deepEqual(context("; [bg"), {kind: "none"});
  assert.deepEqual(context("*lab"), {kind: "none"});
});

test("targets under the cursor", () => {
  const command = targetAt(bundledSdk, "[show guide smile left]", 2);
  assert.equal(command?.kind, "commandName");
  const argument = targetAt(bundledSdk, "[jump scene=park target=gate]", 26);
  assert.ok(argument?.kind === "argument");
  assert.equal(argument.slot, "target");
  assert.deepEqual(argument.columns, [17, 28]);
  const speaker = targetAt(bundledSdk, "@直哉「公園がいいな」", 1);
  assert.ok(speaker?.kind === "speakerName");
  assert.equal(speaker.name, "直哉");
  const option = targetAt(bundledSdk, "@guide[sad voice=g001] 暗いね。", 8);
  assert.ok(option?.kind === "speakerOption");
  assert.equal(option.slot, "expression");
});
