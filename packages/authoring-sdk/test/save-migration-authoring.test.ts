import assert from "node:assert/strict";
import test from "node:test";

import {planSaveAnchorPolicy} from "../src/index.js";
import type {RuntimeIrDocument, RuntimeIrOperation} from "../src/index.js";

function operation(operationId: string, sourceId = operationId): RuntimeIrOperation {
  return {operationId, sourceId, sourceLine: 1, kind: "narration",
    text: operationId, arguments: {}};
}

function runtime(documentId: string, ...operations: RuntimeIrOperation[]): RuntimeIrDocument {
  return {format: "PrismatiXRuntimeIR", schemaRevision: 1,
    documentId, committedRevision: 0, operations};
}

test("save anchor authoring preserves surviving IDs and requires reviewed replacements", () => {
  const old = runtime("chapter", operation("opening"), operation("old-line"));
  const current = runtime("chapter", operation("opening"), operation("new-line"));
  const missing = planSaveAnchorPolicy(old, current);
  assert.equal(missing.valid, false);
  assert.deepEqual(missing.diagnostics.map((diagnostic) => diagnostic.code), ["PXSDKMIG2004"]);
  assert.equal(missing.diagnostics[0]?.sourceId, "old-line");

  const reviewed = planSaveAnchorPolicy(old, current,
    [{fromOperationId: "old-line", toOperationId: "new-line"}]);
  assert.equal(reviewed.valid, true, JSON.stringify(reviewed.diagnostics));
  assert.equal(reviewed.value?.preserved, 1);
  assert.equal(reviewed.value?.mapped, 1);
  assert.deepEqual(reviewed.value?.anchor, {policy: "map-or-preserve", mappings: [{
    from: {runtimeDocumentId: "chapter", sourceId: "old-line", operationId: "old-line"},
    to: {runtimeDocumentId: "chapter", sourceId: "new-line", operationId: "new-line"},
  }]});
  assert.deepEqual(planSaveAnchorPolicy(current, current).value?.anchor,
    {policy: "preserve"});
});

test("save anchor authoring checks the full identity and ambiguous replacements", () => {
  const old = runtime("chapter", operation("line", "old-source"));
  const current = runtime("chapter", operation("line", "new-source"));
  assert.equal(planSaveAnchorPolicy(old, current).diagnostics[0]?.code, "PXSDKMIG2004");
  const reviewed = planSaveAnchorPolicy(old, current,
    [{fromOperationId: "line", toOperationId: "line"}]);
  assert.deepEqual(reviewed.value?.anchor, {policy: "map-or-preserve", mappings: [{
    from: {runtimeDocumentId: "chapter", sourceId: "old-source", operationId: "line"},
    to: {runtimeDocumentId: "chapter", sourceId: "new-source", operationId: "line"},
  }]});

  assert.equal(planSaveAnchorPolicy(old, runtime("new-chapter", operation("line", "old-source")))
    .diagnostics[0]?.code, "PXSDKMIG2004");
  assert.equal(planSaveAnchorPolicy(old, current,
    [{fromOperationId: "line", toOperationId: "absent"}]).diagnostics[0]?.code,
  "PXSDKMIG2002");
  assert.equal(planSaveAnchorPolicy(old, current, [
    {fromOperationId: "line", toOperationId: "line"},
    {fromOperationId: "line", toOperationId: "line"},
  ]).diagnostics[0]?.code, "PXSDKMIG2003");
  assert.equal(planSaveAnchorPolicy(old, runtime("chapter", operation("line"), operation("line")))
    .valid, false);
});
