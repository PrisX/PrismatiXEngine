import assert from "node:assert/strict";
import { pathToFileURL } from "node:url";
import path from "node:path";

const modulePath = process.argv[2];
assert.ok(modulePath, "Usage: node scripts/test-wasm-preview.mjs <prismatix-preview.mjs>");
const absoluteModule = path.resolve(modulePath);
const createPreview = (await import(pathToFileURL(absoluteModule).href)).default;
const module = await createPreview({
  locateFile: (file) => path.join(path.dirname(absoluteModule), file),
});
const probe = module.cwrap("px_preview_test_conformance", "string", ["string"]);
const invoke = (request) => JSON.parse(probe(JSON.stringify(request)));

const unicode = invoke({
  kind: "unicode",
  text: "A👩‍👩‍👧‍👦中 אב",
  language: "ja",
  localeRightToLeft: false,
});
assert.equal(unicode.ok, true);
assert.deepEqual(unicode.clusters, ["A", "👩‍👩‍👧‍👦", "中", " ", "א", "ב"]);
assert.equal(unicode.graphemeBoundaries.at(-1), Buffer.byteLength("A👩‍👩‍👧‍👦中 אב"));
assert.equal(unicode.lineBreakBoundaries.at(-1), Buffer.byteLength("A👩‍👩‍👧‍👦中 אב"));
assert.ok(unicode.directions.some((entry) => entry.rightToLeft));
assert.ok(unicode.directions.some((entry) => !entry.rightToLeft));

const envelope = {
  protocol: "PrismatiXPreviewProtocol",
  schemaRevision: 2,
  protocolVersion: 2,
  type: "apply",
  sessionId: "wasm-ci-session",
  requestId: "wasm-ci-apply-1",
  documentId: "wasm-ci-scene",
  revision: 1,
  payload: {
    runtimeIr: JSON.stringify({
      format: "PrismatiXRuntimeIR",
      schemaRevision: 2,
      documentId: "wasm-ci-scene",
      committedRevision: 1,
      operations: [{
        operationId: "wasm-ci-op",
        sourceId: "wasm-ci-source",
        sourceLine: 1,
        kind: "dialogue",
        text: "@雪: 晚安",
        arguments: { speaker: "雪", text: "晚安" },
      }],
    }),
  },
};
const protocol = invoke({ kind: "protocol", envelope });
assert.equal(protocol.ok, true);
assert.equal(protocol.accepted, true);
assert.equal(protocol.revision, 1);
assert.equal(protocol.events[0].status, "applying");

const stale = structuredClone(envelope);
stale.protocolVersion = 1;
const rejected = invoke({ kind: "protocol", envelope: stale });
assert.equal(rejected.ok, true);
assert.equal(rejected.accepted, false);
assert.ok(rejected.events.some((event) =>
  event.diagnostics?.some((diagnostic) => diagnostic.code === "PXWASM-PROTOCOL-003")));

console.log("PrismatiX WASM Preview protocol and Unicode parity passed");
