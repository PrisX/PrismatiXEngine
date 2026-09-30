import assert from "node:assert/strict";
import {join} from "node:path";
import {test} from "node:test";

import {CliNotFoundError, describePreviewFailure, parseCliLine, previewArguments, resolveCli,
  shellArguments, validateArguments} from "../src/cli-support.js";
import {minimalRoot} from "./helpers.js";

test("Preview From Here passes the scene and one-based line to dev --at", () => {
  assert.deepEqual(previewArguments("C:/game", "intro", 12),
    ["dev", "C:/game", "--at", "intro:12", "--json"]);
  assert.deepEqual(validateArguments("C:/game", ["--ui-registry", "x.json"]),
    ["validate", "C:/game", "--json", "--ui-registry", "x.json"]);
});

test("a CLI without --at is explained instead of failing silently", () => {
  // Exactly what `prismatix dev <project> --at x:1 --json` prints before --at exists.
  const line = parseCliLine(JSON.stringify({severity: "error", code: "PXCLI0001",
    message: "Invalid command arguments", hint: "Error: Unknown option for dev: --at"}));
  assert.equal(line.kind, "diagnostic");
  assert.ok(line.kind === "diagnostic");
  const message = describePreviewFailure(2, [line.diagnostic]);
  assert.match(message, /does not support `prismatix dev --at` yet/u);
  assert.match(message, /Unknown option for dev: --at/u);
  const other = describePreviewFailure(1, [{severity: "error", code: "PXCLI1600",
    message: "Development preview failed", hint: "no runtime"}]);
  assert.equal(other, "Preview failed: PXCLI1600 Development preview failed — no runtime");
});

test("progress events and text are not diagnostics", () => {
  assert.equal(parseCliLine(JSON.stringify({event: "progress", severity: "information", code: "PXCLI1103",
    message: "Parsing Story: 1/2 scenes"})).kind, "event");
  assert.equal(parseCliLine("plain text").kind, "text");
});

test("the project's own CLI is required unless a path is configured", () => {
  assert.throws(() => resolveCli(minimalRoot, "node"), CliNotFoundError);
  const configured = resolveCli(minimalRoot, "node", join("tools", "cli.js"));
  assert.equal(configured.command, "node");
  assert.deepEqual(configured.args, [join("tools", "cli.js")]);
  assert.deepEqual(shellArguments(["dev", "C:/My Game", "--at", "a:1"]), ["dev", "\"C:/My Game\"", "--at", "a:1"]);
});
