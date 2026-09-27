import assert from "node:assert/strict";
import test from "node:test";

import {validatePlayerUiBindings} from "../src/project-ui-bindings.js";
import type {ProjectDocument} from "../src/project.js";
import type {JsonValue} from "../src/types.js";

const rootId = "11111111-1111-4111-8111-111111111111";
const gridId = "22222222-2222-4222-8222-222222222222";
const itemId = "33333333-3333-4333-8333-333333333333";
const path = "Content/UI/Settings.pxui";
const registry = {controls: [{runtimeType: "Label", properties: [
  {id: "text", writable: true, bindable: true, animatable: false, valueType: "string"},
  {id: "visible", writable: true, bindable: true, animatable: false, valueType: "boolean"},
]}]};
const project = (items: unknown): ProjectDocument => ({
  format: "PrismatiXProject", schemaRevision: 2, id: "binding-test",
  name: "Binding Test", version: "0.2.0", contentVersion: "chapter-1",
  saveVersion: 1, resolution: {width: 1280, height: 720},
  entry: {story: "main", ui: "settings"}, gameCatalog: "Content/game.pxgame",
  defaultLocale: "en", supportedLocales: ["en"],
  storyIndex: "Story/story.pxindex", uiEntryPoints: {settings: path},
  extensions: [], settings: {"player.uiData": {"settings.items": items} as JsonValue},
});
const scene = (overrides: Record<string, unknown> = {}): JsonValue => ({
  nodes: [
    {id: rootId, parentId: null},
    {id: gridId, parentId: rootId,
      itemSource: {path: "settings.items", key: "key", placement: "rect"}},
    {id: itemId, parentId: gridId, runtimeType: "Label",
      bindings: {text: {path: "item.title"}}, ...overrides},
  ],
}) as JsonValue;
const validate = (items: unknown, overrides?: Record<string, unknown>) =>
  validatePlayerUiBindings(project(items), new Map([[path, scene(overrides)]]), registry);

test("declared Player item data validates keys, placement, fields, and formatter types", () => {
  const good = [{key: "one", title: "First", enabled: true,
    rect: {x: 1, y: 2, width: 30, height: 40}}];
  assert.deepEqual(validate(good), []);
  assert.ok(validate([{...good[0], key: ""}]).some((item) => item.code === "PXBUILD1081"));
  assert.ok(validate([good[0], {...good[0]}]).some((item) => item.code === "PXBUILD1081"));
  assert.ok(validate([{...good[0], rect: {...good[0]!.rect, width: 0}}])
    .some((item) => item.code === "PXBUILD1082"));
  assert.ok(validate([{...good[0], title: undefined}])
    .some((item) => item.code === "PXBUILD1085"));
  assert.ok(validate(good, {bindings: {text: {path: "item.title", formatter: "Not"}}})
    .some((item) => item.code === "PXBUILD1083"));
  assert.ok(validate(good, {bindings: {visible: {path: "item.title"}}})
    .some((item) => item.code === "PXBUILD1084"));
  assert.ok(validate({unexpected: true}).some((item) => item.code === "PXBUILD1080"));
});

test("declared Player ViewModel binding paths are checked without guessing host paths", () => {
  const data = {"settings.items": [{key: "one", title: "First"}]};
  const direct = {nodes: [{id: rootId, parentId: null, runtimeType: "Label",
    bindings: {text: {path: "settings.items[0].title"}}}]};
  assert.deepEqual(validatePlayerUiBindings(project(data["settings.items"]),
    new Map([[path, direct as JsonValue]]), registry), []);
  const missing = {nodes: [{id: rootId, parentId: null, runtimeType: "Label",
    bindings: {text: {path: "settings.items[0].missing"}}}]};
  assert.ok(validatePlayerUiBindings(project(data["settings.items"]),
    new Map([[path, missing as JsonValue]]), registry)
    .some((item) => item.code === "PXBUILD1086"));
  const hostDefined = {nodes: [{id: rootId, parentId: null, runtimeType: "Label",
    bindings: {text: {path: "dialogue.text"}}}]};
  assert.deepEqual(validatePlayerUiBindings(project(data["settings.items"]),
    new Map([[path, hostDefined as JsonValue]]), registry), []);

  const overlapping = {...project([]), settings: {"player.uiData": {
    screen: {detail: {title: "Fallback"}},
    "screen.detail": {other: "Closer root"},
  }}};
  const fallback = {nodes: [{id: rootId, parentId: null, runtimeType: "Label",
    bindings: {text: {path: "screen.detail.title"}}}]};
  assert.deepEqual(validatePlayerUiBindings(overlapping,
    new Map([[path, fallback as JsonValue]]), registry), []);
});

test("declared host ViewModel and custom formatter contracts validate dynamic item bindings", () => {
  const values = {
    "settings.items": "array",
    "settings.items[].key": "string",
    "settings.items[].rect.x": "number",
    "settings.items[].rect.y": "number",
    "settings.items[].rect.width": "number",
    "settings.items[].rect.height": "number",
    "settings.items[].title": "string",
  };
  const contract = (overrides: Record<string, unknown> = {}) => ({
    ...project([]), settings: {"authoring.uiContract": {
      format: "PrismatiXUiContract", schemaRevision: 1,
      roots: ["settings.items"], values,
      formatters: {TitleCase: {input: "string", output: "string"}},
      ...overrides,
    } as JsonValue},
  });
  const bindings = {bindings: {text: {path: "item.title", formatter: "TitleCase"}}};
  const check = (document: JsonValue, declared = contract()) =>
    validatePlayerUiBindings(declared, new Map([[path, document]]), registry);
  assert.deepEqual(check(scene(bindings)), []);
  assert.ok(check(scene(bindings), contract({values: {...values,
    "settings.items[].title": "number"}})).some((item) => item.code === "PXBUILD1083"));
  const {"settings.items[].title": _title, ...withoutTitle} = values;
  assert.ok(check(scene(bindings), contract({values: withoutTitle}))
    .some((item) => item.code === "PXBUILD1094"));
  const {"settings.items[].key": _key, ...withoutKey} = values;
  assert.ok(check(scene(bindings), contract({values: withoutKey}))
    .some((item) => item.code === "PXBUILD1092"));
  assert.ok(check(scene(bindings), contract({formatters: {}}))
    .some((item) => item.code === "PXBUILD1093"));
  assert.ok(check(scene(bindings), contract({formatters: {
    TitleCase: {input: "string", output: "boolean"},
  }})).some((item) => item.code === "PXBUILD1084"));
});

test("host ViewModel contract checks direct paths without guessing unrelated roots", () => {
  const declared = {...project([]), settings: {"authoring.uiContract": {
    format: "PrismatiXUiContract", schemaRevision: 1,
    roots: ["dialogue"], values: {dialogue: "object", "dialogue.text": "string"},
  } as JsonValue}};
  const direct = (source: string): JsonValue => ({nodes: [
    {id: rootId, parentId: null, runtimeType: "Label",
      bindings: {text: {path: source}}},
  ]}) as JsonValue;
  const check = (source: string) => validatePlayerUiBindings(declared,
    new Map([[path, direct(source)]]), registry);
  assert.deepEqual(check("dialogue.text"), []);
  assert.ok(check("dialogue.missing").some((item) => item.code === "PXBUILD1094"));
  assert.deepEqual(check("host.unknown"), []);
  assert.deepEqual(check("settings.other"), []);
  assert.ok(validatePlayerUiBindings({...declared, settings: {
    "player.uiData": {"dialogue.text": "Ready"},
    "authoring.uiContract": {format: "PrismatiXUiContract", schemaRevision: 1,
      roots: ["dialogue"], values: {dialogue: "object", "dialogue.text": "number"}} as JsonValue,
  }}, new Map([[path, direct("dialogue.text")]]), registry)
    .some((item) => item.code === "PXBUILD1095"));
  assert.ok(validatePlayerUiBindings({...declared, settings: {
    "authoring.uiContract": {format: "PrismatiXUiContract", schemaRevision: 1,
      roots: ["dialogue"], values: {dialogue: "object", "dialogue..text": "string"}} as JsonValue,
  }}, new Map([[path, direct("dialogue.text")]]), registry)
    .some((item) => item.code === "PXBUILD1090"));
});
