import assert from "node:assert/strict";
import test from "node:test";

import {validateHostActionContract, validateUiActionUsage} from "../src/index.js";
import type {ExtensionManifest, HostActionContract} from "../src/index.js";

const extension: ExtensionManifest = {
  format: "PrismatiXExtension", schemaRevision: 1, language: "javascript",
  id: "music-extension", version: "1.0.0", requiredEngineVersion: "^0.1.0",
  entry: "Extensions/music.js", capabilities: ["ui", "audio"], commands: [],
  actions: [{id: "jukebox.play", parameters: [
    {name: "media", type: "resource", required: true},
    {name: "volume", type: "number", range: {minimum: 0, maximum: 1}},
    {name: "mode", type: "string", enum: ["once", "loop"]},
  ]}],
};

const action = (arguments_: Record<string, unknown>) => ({id: "jukebox.play", arguments: arguments_});
const validArguments = {media: {type: "resource", value: "Assets/Audio/theme.ogg"},
  volume: 1, mode: "loop"};

test("extension Action validation accepts typed UI arguments and dynamic signal mappings", () => {
  const document = {nodes: [
    {onClick: action(validArguments)},
    {componentInstance: {publicSignals: {activated: {
      ...action({mode: "once"}), argumentBindings: {media: "resource"},
    }}}},
    {onClick: action({media: {type: "resource", value: "22222222-2222-4222-8222-222222222222"}})},
    {onClick: {id: "player.dynamic", arguments: {anything: true}}},
  ], behaviorGraph: {nodes: [{id: "graph-action", kind: "action",
    properties: {action: "jukebox.play"}, arguments: {mode: "loop"}}],
    links: [{toNodeId: "graph-action", toPin: "arg:media"}]}};
  assert.deepEqual(validateUiActionUsage(document, [extension], "Content/UI/Title.pxui"), []);
});

test("extension Action validation reports every invalid static argument before Player", () => {
  const document = {nodes: [
    {onClick: action({volume: "loud", mode: "invalid", extra: 1})},
    {onClick: action({media: "Assets/Audio/theme.ogg", volume: 2})},
    {onClick: action({media: {type: "resource", value: "../secret.ogg"}})},
    {componentInstance: {publicSignals: {activated: {
      ...action({}), argumentBindings: {unknown: "value"},
    }}}},
  ]};
  const source = JSON.stringify(document, null, 2);
  const diagnostics = validateUiActionUsage(document, [extension],
    "Content/UI/Title.pxui", source);
  const codes = diagnostics.map((diagnostic) => diagnostic.code);
  for (const code of ["PXBUILD1060", "PXBUILD1061", "PXBUILD1062",
    "PXBUILD1063", "PXBUILD1064"])
    assert.ok(codes.includes(code), code);
  assert.ok(diagnostics.every((diagnostic) => diagnostic.path === "Content/UI/Title.pxui"));
  assert.ok(diagnostics.some((diagnostic) => diagnostic.details?.includes("nodes[0].onClick")));
  const wrongVolume = diagnostics.find((diagnostic) => diagnostic.code === "PXBUILD1062" &&
    diagnostic.details?.includes("nodes[0].onClick: jukebox.play.volume"));
  assert.ok(wrongVolume?.span);
  assert.equal(source.slice(wrongVolume.span.start.offset, wrongVolume.span.end.offset), '"loud"');
  const missingMedia = diagnostics.find((diagnostic) => diagnostic.code === "PXBUILD1060" &&
    diagnostic.details?.includes("nodes[0].onClick: jukebox.play.media"));
  assert.ok(missingMedia?.span);
  assert.equal(source.slice(missingMedia.span.start.offset, missingMedia.span.end.offset),
    '"jukebox.play"');
  const stale = validateUiActionUsage(document, [extension],
    "Content/UI/Title.pxui", source.replace('"loud"', '"quiet"'));
  assert.ok(stale.every((diagnostic) => diagnostic.span === undefined));
});

test("item-template Action placeholders are checked against extension parameter types", () => {
  const itemExtension: ExtensionManifest = {...extension, actions: [{
    id: "item.select", parameters: [
      {name: "index", type: "integer", required: true},
      {name: "key", type: "string", required: true},
    ],
  }]};
  const document = {nodes: [
    {id: "grid", itemSource: {path: "items", key: "key"}},
    {id: "template", parentId: "grid"},
    {id: "button", parentId: "template", onClick: {
      id: "item.select", arguments: {index: "$index", key: "$key"},
    }},
  ]};
  assert.deepEqual(validateUiActionUsage(document, [itemExtension], "Items.pxui"), []);
  const invalidExtension: ExtensionManifest = {...itemExtension, actions: [{
    id: "item.select", parameters: [
      {name: "index", type: "boolean", required: true},
      {name: "key", type: "string", required: true},
    ],
  }]};
  assert.ok(validateUiActionUsage(document, [invalidExtension], "Items.pxui")
    .some((diagnostic) => diagnostic.code === "PXBUILD1062"));
});

test("host Action declarations reuse typed UI argument validation", () => {
  const host: HostActionContract = {format: "PrismatiXActionContract", schemaRevision: 1,
    actions: [{id: "host.open", safety: {previewSafe: true,
      deterministic: true, seekSafe: true, rollbackSafe: true},
    parameters: [{name: "target", type: "string", required: true}]}]};
  assert.deepEqual(validateHostActionContract(host, "prismatix.json"), []);
  const document = {nodes: [{onClick: {id: "host.open", arguments: {target: 7}}}]};
  assert.equal(validateUiActionUsage(document, [], "Title.pxui", undefined,
    host.actions)[0]?.code, "PXBUILD1062");
  assert.equal(validateUiActionUsage({nodes: [{onClick: {
    id: "host.open", arguments: {target: "settings"}}}]}, [], "Title.pxui",
  undefined, host.actions).length, 0);
  assert.ok(validateHostActionContract({...host, actions: [
    host.actions[0]!, host.actions[0]!,
  ]}, "prismatix.json").some((item) => item.code === "PXSDKSEM1107"));
});
