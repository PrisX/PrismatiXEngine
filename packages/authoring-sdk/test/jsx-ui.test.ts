import assert from "node:assert/strict";
import test from "node:test";

import {
  Button,
  Component,
  Control,
  Image,
  Scene,
  Text,
  VBox,
  compileJsxUi,
} from "../src/index.js";
import {jsx, jsxs, type UiElement} from "../src/jsx-runtime.js";

const sceneId = "90000000-0000-4000-8000-000000000001";

function Card(props: {readonly title: string}): UiElement {
  return jsxs(VBox, {name: "Card", layout: {width: 320, height: 180}, children: [
    jsx(Text, {name: "CardTitle", children: props.title,
      bindings: {text: {path: "locale.card.title"}}}),
    jsx(Button, {name: "Confirm", children: "Confirm",
      onClick: {id: "dialog.confirm", arguments: {source: "card"}}}),
  ]});
}

function authoredScene(): UiElement {
  return jsx(Scene, {id: sceneId, name: "JSX Example", width: 1280, height: 720,
    theme: [{id: "paper", name: "Paper", value: "#fff"}],
    children: jsxs(Control, {name: "Root", appearance: {styleToken: "paper"}, children: [
      jsx(Image, {name: "Backdrop", assetId: "91000000-0000-4000-8000-000000000001",
        layout: {width: 1280, height: 720}}),
      jsx(Card, {title: "A deterministic card"}),
    ]}),
  });
}

test("React-free JSX lowering produces canonical UI with deterministic node ids", () => {
  const first = compileJsxUi(authoredScene(), {path: "Content/UI/Example.tsx", kind: "scene"});
  const second = compileJsxUi(authoredScene(), {path: "Content/UI/Example.tsx", kind: "scene"});
  assert.equal(first.valid, true, JSON.stringify(first.diagnostics));
  assert.equal(second.valid, true, JSON.stringify(second.diagnostics));
  assert.deepEqual(second.value, first.value);
  assert.equal(first.value?.format, "PrismatiXUIScene");
  assert.equal(first.value?.id, sceneId);
  assert.equal(first.value?.nodes.length, 5);
  const card = first.value?.nodes.find((node) => node.name === "Card");
  const title = first.value?.nodes.find((node) => node.name === "CardTitle");
  const button = first.value?.nodes.find((node) => node.name === "Confirm");
  assert.equal(card?.kind, "vbox");
  assert.equal(card?.runtimeType, "VBoxContainer");
  assert.equal(title?.layout.mode, "container");
  assert.deepEqual(title?.bindings, {text: {path: "locale.card.title"}});
  assert.deepEqual(button?.onClick, {id: "dialog.confirm", arguments: {source: "card"}});
});

test("JSX supports reusable component documents through the canonical validator", () => {
  const component = jsx(Component, {
    name: "Panel", width: 400, height: 240,
    children: jsx(VBox, {name: "PanelRoot", children: jsx(Text, {
      name: "Caption", text: "Panel",
    })}),
  });
  const result = compileJsxUi(component, {kind: "component"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.value?.format, "PrismatiXUIComponent");
  assert.match(result.value!.id,
    /^[0-9a-f]{8}-[0-9a-f]{4}-5[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/u);
  if (result.value?.format === "PrismatiXUIComponent") {
    assert.deepEqual(result.value.componentInterface,
      {properties: [], signals: [], slots: []});
  }
});

test("document and duplicate-name sibling ids are automatic, unique, and deterministic", () => {
  const authored = () => jsx(Scene, {
    name: "Automatic IDs", width: 640, height: 360,
    children: jsxs(VBox, {name: "Root", children: [
      jsx(Text, {name: "Status", children: "First"}),
      jsx(Text, {name: "Status", children: "Second"}),
      jsx(Text, {stableId: "semantic-status", children: "Stable"}),
    ]}),
  });
  const first = compileJsxUi(authored(), {kind: "scene"});
  const second = compileJsxUi(authored(), {kind: "scene"});
  assert.equal(first.valid, true, JSON.stringify(first.diagnostics));
  assert.deepEqual(second.value, first.value);
  assert.match(first.value!.id,
    /^[0-9a-f]{8}-[0-9a-f]{4}-5[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/u);
  const statuses = first.value!.nodes.filter((value) => value.name === "Status");
  assert.equal(statuses.length, 2);
  assert.notEqual(statuses[0]!.id, statuses[1]!.id);
  assert.equal(new Set(first.value!.nodes.map((value) => value.id)).size,
    first.value!.nodes.length);
});

test("layout, style, binding, and action shorthands lower to canonical fields", () => {
  const result = compileJsxUi(jsx(Scene, {
    name: "Shorthand", width: 800, height: 600,
    children: jsx(Control, {name: "Root", fill: true, children: jsx(Button, {
      name: "Save", position: [24, 32], size: [180, 48], anchor: "bottom-right",
      pivot: [1, 1], align: "center", margin: 8,
      style: {token: "primary", background: "#112233", color: "#ffffff",
        hoverBackground: "#223344", focus: "#abcdef", opacity: 0.9},
      bind: {visible: "game.canSave"}, action: "save.open", children: "Save",
    })}),
  }), {kind: "scene"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const root = result.value!.nodes[0]!;
  const button = result.value!.nodes[1]!;
  assert.equal(root.layout.anchorRight, 1);
  assert.equal(root.layout.sizeRule, "fill");
  assert.deepEqual(button.layout, {
    mode: "free", x: 24, y: 32, width: 180, height: 48,
    anchorX: 1, anchorY: 1, anchorRight: 1, anchorBottom: 1,
    pivotX: 1, pivotY: 1, margin: 8, alignment: "center", sizeRule: "content",
  });
  assert.deepEqual(button.appearance, {
    backgroundColor: "#112233", textColor: "#ffffff", opacity: 0.9,
    styleToken: "primary", hoverBackgroundColor: "#223344", focusColor: "#abcdef",
  });
  assert.deepEqual(button.bindings, {visible: {path: "game.canSave"}});
  assert.deepEqual(button.onClick, {id: "save.open", arguments: {}});
});
