import assert from "node:assert/strict";
import test from "node:test";

import {parseStoryExpression} from "../src/story-expression.js";
import type {GameVariable} from "../src/types.js";

const variables: readonly GameVariable[] = [
  {name: "affection", type: "integer", default: 0, scope: "profile"},
  {name: "route", type: "string", default: "common", scope: "session"},
  {name: "unlocked", type: "boolean", default: false, scope: "profile"},
];

test("Story expressions lower precedence and parentheses to typed runtime AST", () => {
  const parsed = parseStoryExpression(
    '(affection + 2 >= 5 && route == "rin") || unlocked',
    variables,
  );
  assert.equal(parsed.error, undefined);
  assert.deepEqual(JSON.parse(parsed.encoded!), {
    kind: "binary",
    op: "or",
    left: {
      kind: "binary",
      op: "and",
      left: {
        kind: "binary",
        op: "greater-equal",
        left: {
          kind: "binary",
          op: "add",
          left: {kind: "variable", name: "affection"},
          right: {kind: "literal", value: 2},
        },
        right: {kind: "literal", value: 5},
      },
      right: {
        kind: "binary",
        op: "equal",
        left: {kind: "variable", name: "route"},
        right: {kind: "literal", value: "rin"},
      },
    },
    right: {kind: "variable", name: "unlocked"},
  });
});

test("Story expressions preserve the legacy unquoted comparison string", () => {
  const parsed = parseStoryExpression("route == rin", variables);
  assert.equal(parsed.error, undefined);
  assert.deepEqual(JSON.parse(parsed.encoded!), {
    kind: "binary",
    op: "equal",
    left: {kind: "variable", name: "route"},
    right: {kind: "literal", value: "rin"},
  });
});

test("Story expressions fail during authoring for invalid variables, syntax, and types", () => {
  assert.match(parseStoryExpression("missing > 1", variables).error ?? "", /Unknown game variable/u);
  assert.match(parseStoryExpression("affection && unlocked", variables).error ?? "", /boolean operands/u);
  assert.match(parseStoryExpression("(unlocked", variables).error ?? "", /Expected '\)'/u);
  assert.match(parseStoryExpression("route + 1 == 2", variables).error ?? "", /numeric operands/u);
});
