import assert from "node:assert/strict";
import test from "node:test";

import type {GameDocument} from "../src/types.js";

const empty = {format: "PrismatiXGame", variables: [], gallery: [],
  unlockables: []} as const;

const revision2: GameDocument = {...empty, schemaRevision: 2,
  inputActions: [{id: "quick-save", context: "game", keys: ["F8"]}]};
const revision3: GameDocument = {...empty, schemaRevision: 3,
  inputActions: [{id: "settings.keys", context: "ui", keys: ["F2"],
    uiAction: {command: "settings.page7"}}]};

// @ts-expect-error UI command shortcuts cannot be authored as revision 2
const invalid = {...empty, schemaRevision: 2, inputActions: [{id: "settings.keys", context: "ui", uiAction: {command: "settings.page7"}}]} satisfies GameDocument;

test("game input action revisions are discriminated in TypeScript", () => {
  assert.equal(revision2.schemaRevision, 2);
  assert.equal(revision3.schemaRevision, 3);
  assert.equal(invalid.schemaRevision, 2);
});
