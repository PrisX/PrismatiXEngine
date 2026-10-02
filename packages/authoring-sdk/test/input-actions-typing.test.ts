import assert from "node:assert/strict";
import test from "node:test";

import type {GameDocument} from "../src/types.js";

const empty = {format: "PrismatiXGame", variables: [], gallery: [],
  unlockables: []} as const;

const keyed: GameDocument = {...empty, schemaRevision: 1,
  inputActions: [{id: "quick-save", context: "game", keys: ["F8"]}]};
const shortcut: GameDocument = {...empty, schemaRevision: 1,
  inputActions: [{id: "settings.keys", context: "ui", keys: ["F2"],
    uiAction: {command: "settings.page7"}}]};

test("game input actions are typed, including UI command shortcuts", () => {
  assert.equal(keyed.schemaRevision, 1);
  assert.equal(shortcut.schemaRevision, 1);
});
