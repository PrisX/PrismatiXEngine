import {canonicalJson} from "./canonical-json.js";
import {parseStory} from "./story.js";
import type {JsonValue, Result} from "./types.js";

const identifier = /^[A-Za-z0-9][A-Za-z0-9._-]*$/u;

/** Encode a named-argument Story command for format-specific importers. The
 * returned line can be inserted into a .pxstory source. `if` expressions are
 * deliberately excluded because their grammar is not named arguments. */
export function serializeStoryCommand(name: string,
  arguments_: Readonly<Record<string, JsonValue>> = {}): Result<string> {
  if (!identifier.test(name) || name === "if") return {valid: false,
    diagnostics: [{severity: "error", code: "PXIMPORT1002",
      message: "Imported Story command name is invalid or requires expression syntax",
      details: name}]};
  if (arguments_ === null || typeof arguments_ !== "object" || Array.isArray(arguments_))
    return {valid: false, diagnostics: [{severity: "error", code: "PXIMPORT1004",
      message: "Imported Story arguments must be an object"}]};
  const tokens: string[] = [];
  for (const [key, value] of Object.entries(arguments_)) {
    if (!identifier.test(key)) return {valid: false,
      diagnostics: [{severity: "error", code: "PXIMPORT1003",
        message: "Imported Story argument name must be a stable identifier",
        details: key}]};
    let encoded: string;
    try { encoded = canonicalJson(value, false).trimEnd(); }
    catch (cause) { return {valid: false, diagnostics: [{severity: "error",
      code: "PXIMPORT1004", message: "Imported Story argument is not a JSON value",
      details: `${key}: ${cause instanceof Error ? cause.message : String(cause)}`}]}; }
    tokens.push(`${key}=${encoded}`);
  }
  const source = `[${name}${tokens.length === 0 ? "" : ` ${tokens.join(" ")}`}]`;
  const parsed = parseStory(source);
  if (parsed.diagnostics.some((diagnostic) => diagnostic.severity === "error"))
    return {valid: false, diagnostics: parsed.diagnostics};
  return {valid: true, value: source, diagnostics: []};
}
