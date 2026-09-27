import assert from "node:assert/strict";
import test from "node:test";

import {jsonPointerSpans} from "../src/json-source-spans.js";

test("JSON source spans keep UTF-16 columns and UTF-8 byte offsets", () => {
  const source = `{
  "title": "雪🌸",
  "settings": {"name": "舊", "name": "新值"}
}`;
  const document = JSON.parse(source) as unknown;
  const span = jsonPointerSpans(source, document, "prismatix.json")
    .get("/settings/name");
  assert.ok(span);
  const utf16Start = source.lastIndexOf('"新值"');
  assert.equal(span.path, "prismatix.json");
  assert.equal(span.start.line, 3);
  assert.equal(span.start.column,
    utf16Start - source.lastIndexOf("\n", utf16Start));
  assert.equal(span.start.offset,
    Buffer.byteLength(source.slice(0, utf16Start), "utf8"));
  assert.equal(Buffer.from(source).subarray(span.start.offset,
    span.end.offset).toString("utf8"), '"新值"');
  assert.equal(jsonPointerSpans(source, {settings: {name: "舊"}},
    "prismatix.json").size, 0);
});
