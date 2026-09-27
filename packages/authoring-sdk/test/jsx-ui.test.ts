import assert from "node:assert/strict";
import test from "node:test";

import {
  Button,
  Component,
  Control,
  Image,
  ItemsGrid,
  ItemsPlaced,
  KeyBindingButton,
  Modal,
  Motion,
  RichTextLabel,
  Scene,
  SpriteText,
  Text,
  TextureButton,
  VBox,
  WheelBindingButton,
  compileJsxUi,
  validateDocument,
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

test("Modal authors a native focus scope, backdrop action, and blocking panel", () => {
  const scene = jsx(Scene, {name: "Modal demo", width: 1280, height: 720,
    children: jsx(Modal, {name: "Confirm", fill: true, visible: false,
      bind: {visible: "confirm.visible"},
      panel: {x: 390, y: 240, width: 500, height: 240},
      backdropPath: "Assets/UI/scrim.png", backdropAction: "confirm.cancel",
      children: jsx(Button, {name: "Accept", x: 20, y: 30,
        width: 120, height: 40, action: "confirm.accept", children: "OK"}),
    })});
  const compiled = compileJsxUi(scene, {path: "Content/UI/Modal.tsx", kind: "scene"});
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  const modal = compiled.value?.nodes.find((node) => node.name === "Confirm");
  const backdrop = compiled.value?.nodes.find((node) => node.name === "ModalBackdrop");
  const panel = compiled.value?.nodes.find((node) => node.name === "ModalPanel");
  const accept = compiled.value?.nodes.find((node) => node.name === "Accept");
  assert.equal(modal?.runtimeProperties?.modalCapture, true);
  assert.equal(modal?.bindings?.visible?.path, "confirm.visible");
  assert.equal(backdrop?.parentId, modal?.id);
  assert.equal(backdrop?.layout.width, 1280);
  assert.equal(backdrop?.layout.height, 720);
  assert.equal(backdrop?.onClick?.id, "confirm.cancel");
  assert.equal(backdrop?.runtimeProperties?.normalPath, "Assets/UI/scrim.png");
  assert.equal(panel?.parentId, modal?.id);
  assert.equal(panel?.runtimeProperties?.mouseFilter, "Stop");
  assert.equal(accept?.parentId, panel?.id);
  assert.equal(accept?.layout.x, 20);

  const malformed = JSON.parse(JSON.stringify(compiled.value)) as {
    nodes: {name: string; layout: {width: number}}[]};
  const malformedBackdrop = malformed.nodes.find((node) => node.name === "ModalBackdrop");
  assert.ok(malformedBackdrop);
  malformedBackdrop.layout.width = 0;
  assert.equal(validateDocument("ui", malformed, "Content/UI/Modal.pxui").valid, false);
});

test("nested fill uses its parent dimensions instead of an invalid zero-sized layout", () => {
  const scene = jsx(Scene, {name: "Nested fill", width: 1280, height: 720,
    children: jsx(Control, {name: "Root", width: 500, height: 240,
      children: jsx(Button, {name: "FillChild", fill: true, children: "Click"})})});
  const compiled = compileJsxUi(scene, {path: "Content/UI/Nested.tsx", kind: "scene"});
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  const child = compiled.value?.nodes.find((node) => node.name === "FillChild");
  assert.equal(child?.layout.width, 500);
  assert.equal(child?.layout.height, 240);
});

test("Motion lowers a layered clip reference without confusing playhead and layout position", () => {
  const result = compileJsxUi(jsx(Scene, {
    name: "Motion demo", width: 320, height: 120,
    children: jsx(Control, {name: "Root", children: jsx(Motion, {
      name: "Logo", path: "Content/Motion/logo.pxmotion",
      position: [20, 10], size: [280, 100], playhead: 0.25, speed: 1.5,
    })}),
  }), {kind: "scene"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const logo = result.value?.nodes.find((node) => node.name === "Logo");
  assert.equal(logo?.runtimeType, "MotionRect");
  assert.equal(logo?.layout.x, 20);
  assert.equal(logo?.runtimeProperties?.path, "Content/Motion/logo.pxmotion");
  assert.equal(logo?.runtimeProperties?.position, 0.25);
  assert.equal(logo?.runtimeProperties?.speed, 1.5);
});

test("SpriteText authors one typed glyph map and rejects unsafe or repeated entries", () => {
  const glyphs = [
    {character: "1", path: "Assets/Glyph/one.png", x: 0, y: 2, width: 18, height: 30},
    {character: "-", path: "Assets/Glyph/dash.png", x: 1, y: 3, width: 19, height: 10},
  ];
  const author = (entries: typeof glyphs) => compileJsxUi(jsx(Scene, {
    name: "Sprite demo", width: 320, height: 120,
    children: jsx(SpriteText, {name: "ChapterNumber", text: "1-", glyphs: entries,
      advance: 24, position: [0, 0], size: [320, 120],
      bind: {text: "chapter.number"}}),
  }), {kind: "scene"});
  const result = author(glyphs);
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const sprite = result.value?.nodes.find((node) => node.name === "ChapterNumber");
  assert.equal(sprite?.runtimeType, "SpriteText");
  assert.equal(sprite?.runtimeProperties?.advance, 24);
  assert.deepEqual(sprite?.runtimeProperties?.glyphs, glyphs);
  assert.equal(sprite?.bindings.text?.path, "chapter.number");
  assert.equal(author([glyphs[0]!, {...glyphs[1]!, character: "1"}]).valid, false);
  assert.equal(author([{...glyphs[0]!, path: "../escape.png"}]).valid, false);
});

test("leaf and sibling JSX roots get one stable transparent structural root", () => {
  const logo = () => jsx(Motion, {name: "Logo", path: "Content/Motion/logo.pxmotion",
    fill: true});
  const single = compileJsxUi(jsx(Scene, {name: "Title", width: 320, height: 120,
    children: logo()}), {path: "Content/UI/Title.tsx"});
  const siblings = compileJsxUi(jsx(Scene, {name: "Title", width: 320, height: 120,
    children: [logo(), jsx(Button, {name: "Start", action: "game.start", children: "Start"})]}),
  {path: "Content/UI/Title.tsx"});
  assert.equal(single.valid, true, JSON.stringify(single.diagnostics));
  assert.equal(siblings.valid, true, JSON.stringify(siblings.diagnostics));
  const root = single.value!.nodes[0]!;
  assert.equal(root.kind, "group");
  assert.equal(root.runtimeType, "Control");
  assert.equal(root.parentId, null);
  assert.equal(root.layout.sizeRule, "fill");
  assert.equal(root.layout.anchorRight, 1);
  assert.equal(single.value!.nodes[1]!.parentId, root.id);
  assert.equal(siblings.value!.nodes[0]!.id, root.id);
  assert.equal(siblings.value!.nodes[1]!.id, single.value!.nodes[1]!.id);
  assert.equal(siblings.value!.nodes[2]!.parentId, root.id);
  assert.equal(root.appearance, undefined);
});

test("reusable components also accept a leaf JSX root", () => {
  const result = compileJsxUi(jsx(Component, {name: "Badge", width: 160, height: 48,
    children: jsx(Text, {name: "Caption", children: "Badge"})}), {kind: "component"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.value?.nodes[0]?.kind, "group");
  assert.equal(result.value?.nodes[1]?.name, "Caption");
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

test("one authored item template lowers to a keyed Runtime grid with item bindings", () => {
  const authored = jsx(Scene, {name: "Slots", width: 800, height: 600,
    children: jsx(Control, {name: "Root", children: jsx(ItemsGrid, {
      name: "Slots", items: {path: "saveLoad.slots", key: "key"},
      columns: 4, itemExtent: [252, 228], gap: [32, 18],
      children: jsx(Control, {name: "SlotTemplate", width: 252, height: 228,
        children: jsx(Button, {name: "SlotButton", bind: {text: "item.title"},
          action: {id: "save.slot", arguments: {slot: "$index"}}}),
      }),
    })}),
  });
  const result = compileJsxUi(authored, {kind: "scene"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const grid = result.value!.nodes.find((node) => node.name === "Slots")!;
  const template = result.value!.nodes.find((node) => node.name === "SlotTemplate")!;
  const button = result.value!.nodes.find((node) => node.name === "SlotButton")!;
  assert.deepEqual(grid.itemSource, {path: "saveLoad.slots", key: "key"});
  assert.deepEqual(grid.runtimeProperties, {columns: 4,
    itemExtent: {type: "vec2", x: 252, y: 228},
    gap: {type: "vec2", x: 32, y: 18}});
  assert.equal(template.parentId, grid.id);
  assert.deepEqual(button.bindings, {text: {path: "item.title"}});
  assert.deepEqual(button.onClick, {id: "save.slot", arguments: {slot: "$index"}});

  const unscoped = compileJsxUi(jsx(Scene, {name: "Unscoped", width: 800, height: 600,
    children: jsx(Control, {name: "Root", children: jsx(Button, {
      name: "Invalid", action: {id: "save.slot", arguments: {slot: "$index"}},
    })}),
  }), {kind: "scene"});
  assert.equal(unscoped.valid, false);
  assert.ok(unscoped.diagnostics.some((item) => item.code === "PXSDKUI1025"));
});

test("placed item template preserves per-item rectangle source", () => {
  const authored = jsx(Scene, {name: "Placed settings", width: 800, height: 600,
    children: jsx(Control, {name: "Root", children: jsx(ItemsPlaced, {
      name: "Switches", x: 100, y: 80, width: 500, height: 400,
      items: {path: "settings.switches", key: "key", placement: "rect"},
      children: jsx(Control, {name: "SwitchTemplate", width: 388, height: 35,
        children: jsx(Button, {name: "SwitchOn", text: "ON"}),
      }),
    })}),
  });
  const result = compileJsxUi(authored, {kind: "scene"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const placed = result.value!.nodes.find((node) => node.name === "Switches")!;
  assert.equal(placed.runtimeType, "PlacedItemsView");
  assert.deepEqual(placed.itemSource,
    {path: "settings.switches", key: "key", placement: "rect"});
  assert.equal(result.value!.nodes.find((node) => node.name === "SwitchTemplate")!.parentId,
    placed.id);

  const invalid = compileJsxUi(jsx(Scene, {name: "Bad", width: 800, height: 600,
    children: jsx(Control, {name: "Root", children: jsx(ItemsGrid, {
      name: "Grid", items: {path: "settings.switches", key: "key", placement: "rect"},
      children: jsx(Control, {name: "Item"}),
    })}),
  }), {kind: "scene"});
  assert.equal(invalid.valid, false);
  assert.ok(invalid.diagnostics.some((item) => item.code === "PXSDKUI1190"));
});

test("texture buttons and rich labels lower typed convenience props to native controls", () => {
  const result = compileJsxUi(jsx(Scene, {
    name: "Image UI", width: 800, height: 600,
    children: jsxs(Control, {name: "Root", children: [
      jsx(TextureButton, {
        name: "Start", normalPath: "Assets/UI/start.png", hoverPath: "Assets/UI/start-hover.png",
        scaleMode: "Fit", focusMode: "All", hoverSoundPath: "Assets/Audio/hover.ogg",
        activateSoundPath: "Assets/Audio/start.ogg", fontSize: 22,
        horizontalAlignment: "Left", verticalAlignment: "Bottom",
        action: "game.start", children: "Start",
      }),
      jsx(RichTextLabel, {
        name: "Dialogue", markup: "<ruby>花<rt>はな</rt></ruby>", vertical: true,
        verticalRows: 4, fontSize: 30, wrap: true, outlineSize: 2,
        outlineColor: "#2D171DFF", horizontalAlignment: "Center",
      }),
      jsx(Image, {name: "Backdrop", path: "Assets/UI/backdrop.png", scaleMode: "Stretch",
        lockAspectRatio: false, opacity: 0.8, runtimeProperties: {path: "old.png"}}),
    ]}),
  }), {kind: "scene"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const button = result.value!.nodes.find((item) => item.name === "Start")!;
  const label = result.value!.nodes.find((item) => item.name === "Dialogue")!;
  const image = result.value!.nodes.find((item) => item.name === "Backdrop")!;
  assert.equal(button.runtimeType, "TextureButton");
  assert.equal(button.text, "Start");
  assert.deepEqual(button.runtimeProperties, {
    focusMode: "All", normalPath: "Assets/UI/start.png",
    hoverPath: "Assets/UI/start-hover.png", scaleMode: "Fit",
    hoverSoundPath: "Assets/Audio/hover.ogg", activateSoundPath: "Assets/Audio/start.ogg",
    fontSize: 22, horizontalAlignment: "Left", verticalAlignment: "Bottom",
  });
  assert.equal(label.runtimeType, "RichTextLabel");
  assert.equal(label.text, "<ruby>花<rt>はな</rt></ruby>");
  assert.deepEqual(label.runtimeProperties, {fontSize: 30, wrap: true, outlineSize: 2,
    outlineColor: {type: "color", value: "#2D171DFF"}, horizontalAlignment: "Center",
    vertical: true, verticalRows: 4});
  assert.equal(image.runtimeType, "TextureRect");
  assert.deepEqual(image.runtimeProperties, {path: "Assets/UI/backdrop.png", scaleMode: "Stretch",
    lockAspectRatio: false, opacity: 0.8});
});

test("key binding button lowers native capture properties without a click action", () => {
  const result = compileJsxUi(jsx(Scene, {name: "Keyboard Settings",
    width: 800, height: 600,
    children: jsx(KeyBindingButton, {name: "PhotoModeKey",
      bindingAction: "photo-mode", bindingPrompt: "Press a key...",
      unboundText: "Not set", bindingConflictText: "In use",
      bindingConfirmText: "Click again to share",
      bindingLimitText: "Maximum keys reached",
      escapeCancelsBinding: true, allowSharedBinding: false,
      confirmSharedBinding: true,
      deleteClearsBinding: true, addCapturedBinding: true,
      selectableBindingKeys: true,
      normalPath: "Assets/UI/key.png", runtimeProperties: {fontSize: 18},
    }),
  }), {kind: "scene"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const button = result.value!.nodes.find((item) => item.name === "PhotoModeKey")!;
  assert.equal(button.runtimeType, "TextureButton");
  assert.equal(button.onClick, undefined);
  assert.deepEqual(button.runtimeProperties, {fontSize: 18,
    normalPath: "Assets/UI/key.png", bindingAction: "photo-mode",
    bindingPrompt: "Press a key...", unboundText: "Not set",
    bindingConflictText: "In use", bindingLimitText: "Maximum keys reached",
    bindingConfirmText: "Click again to share",
    escapeCancelsBinding: true, allowSharedBinding: false,
    confirmSharedBinding: true,
    deleteClearsBinding: true, addCapturedBinding: true,
    selectableBindingKeys: true});
});

test("wheel binding button lowers direction capture and threshold properties", () => {
  const result = compileJsxUi(jsx(Scene, {name: "Wheel Settings",
    width: 800, height: 600,
    children: jsx(WheelBindingButton, {name: "BacklogWheel",
      bindingAction: "backlog", wheelThreshold: 1.5,
      wheelUpText: "Scroll up", wheelDownText: "Scroll down",
      deleteClearsBinding: true, normalPath: "Assets/UI/wheel.png",
    }),
  }), {kind: "scene"});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const button = result.value!.nodes.find((item) => item.name === "BacklogWheel")!;
  assert.equal(button.runtimeType, "TextureButton");
  assert.equal(button.onClick, undefined);
  assert.deepEqual(button.runtimeProperties, {
    normalPath: "Assets/UI/wheel.png", bindingAction: "backlog",
    bindingDevice: "wheel", bindingPrompt: "Scroll up or down",
    wheelUpText: "Scroll up", wheelDownText: "Scroll down",
    wheelThreshold: 1.5, deleteClearsBinding: true,
  });
});
