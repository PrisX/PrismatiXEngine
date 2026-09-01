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
    id: "92000000-0000-4000-8000-000000000001",
    name: "Panel", width: 400, height: 240,
    children: jsx(VBox, {name: "PanelRoot", children: jsx(Text, {
      name: "Caption", text: "Panel",
    })}),
  });
  const result = compileJsxUi(component, {kind: "component"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.value?.format, "PrismatiXUIComponent");
  if (result.value?.format === "PrismatiXUIComponent") {
    assert.deepEqual(result.value.componentInterface,
      {properties: [], signals: [], slots: []});
  }
});
