import {canonicalJson} from "./canonical-json.js";
import {externalizeStoryIds, storyIdentityMapMarker,
  type StoryIdentityMap} from "./story-external-identity.js";
import {isSafeResourcePath} from "./resources.js";
import {parseStory} from "./story.js";
import type {JsonValue, Result, StoryNode} from "./types.js";

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

/** One decoded source line, already translated into PrismatiX Story syntax.
 * Every executable line needs an adapter-assigned ID stable across imports. */
export interface ImportedStoryLine {
  readonly source: string;
  readonly id?: string;
}

export interface ImportedStorySourceOptions {
  /** Recreate the legacy generated-ID seed for labels without explicit IDs.
   * Keep synchronizing against an existing sidecar to retain IDs changed by
   * earlier edits. Dialogue and commands still need explicit IDs. */
  readonly legacyLabelSourceIds?: {readonly documentId: string};
}

export interface ImportedStorySource {
  readonly source: string;
  readonly identityMap: StoryIdentityMap;
}

function executable(node: StoryNode): boolean {
  return node.kind !== "comment" && node.kind !== "speaker" &&
    !(node.kind === "command" && (node.name === "id" || node.name === "choice.wait"));
}

/** Assemble a format adapter's translated lines into a Story source and
 * adjacent identity map. No source-format decoder or filesystem access is
 * bundled here; callers can stream/decode their own files one scene at a time.
 * The output should be written to `path` and `${path}.pxids.json` together. */
export function buildImportedStorySource(path: string,
  lines: readonly ImportedStoryLine[],
  options: ImportedStorySourceOptions = {}): Result<ImportedStorySource> {
  if (typeof path !== "string" || !isSafeResourcePath(path) || !path.endsWith(".pxstory") ||
      path.includes(":") || /[\u0000-\u001f\u007f]/u.test(path) ||
      path.split("/").some((part) => part.length === 0 || part === "."))
    return {valid: false, diagnostics: [{severity: "error", code: "PXIMPORT1005",
      message: "Imported Story path must be a safe project-relative .pxstory path", path}]};
  if (!Array.isArray(lines)) return {valid: false, diagnostics: [{
    severity: "error", code: "PXIMPORT1006",
    message: "Imported Story lines must be an array", path}]};
  const legacyDocumentId = options.legacyLabelSourceIds?.documentId;
  if (options.legacyLabelSourceIds !== undefined &&
      (typeof legacyDocumentId !== "string" || legacyDocumentId.length === 0 ||
       legacyDocumentId.length > 256))
    return {valid: false, diagnostics: [{severity: "error", code: "PXIMPORT1021",
      message: "Legacy label source IDs require a stable document ID", path}]};
  for (const [index, line] of lines.entries()) {
    if (typeof line?.source !== "string" || /[\r\n\u0000]/u.test(line.source) ||
        line.source.trim() === storyIdentityMapMarker ||
        (line.id !== undefined && (typeof line.id !== "string" || !identifier.test(line.id))))
      return {valid: false, diagnostics: [{severity: "error", code: "PXIMPORT1006",
        message: "Imported Story line or stable ID is invalid", path,
        details: `line ${index + 1}`}]};
  }
  const raw = `${lines.map((line) => line.source).join("\n")}\n`;
  if (Buffer.byteLength(raw, "utf8") > 16 * 1024 * 1024)
    return {valid: false, diagnostics: [{severity: "error", code: "PXIMPORT1007",
      message: "Imported Story source exceeds the 16 MiB source limit", path}]};
  const document = parseStory(raw, path);
  if (document.diagnostics.some((diagnostic) => diagnostic.severity === "error"))
    return {valid: false, diagnostics: document.diagnostics};
  const byLine = new Map<number, StoryNode[]>();
  for (const node of document.nodes) {
    if (node.kind === "command" && node.name === "id") return {valid: false,
      diagnostics: [{severity: "error", code: "PXIMPORT1008",
        message: "Imported Story lines must not contain inline [id] directives", path,
        span: node.span}]};
    if (!executable(node)) continue;
    const line = node.span.start.line;
    byLine.set(line, [...byLine.get(line) ?? [], node]);
  }
  for (const [index, line] of lines.entries()) {
    const nodes = byLine.get(index + 1) ?? [];
    if (nodes.length !== (line.id === undefined ? 0 : 1) &&
        !(legacyDocumentId !== undefined && line.id === undefined &&
          nodes.length === 1 && nodes[0]?.kind === "label")) return {valid: false,
      diagnostics: [{severity: "error", code: "PXIMPORT1009",
        message: "Each executable imported Story line needs exactly one stable ID", path,
        details: `line ${index + 1}`,
        ...(nodes[0] === undefined ? {} : {span: nodes[0].span})}]};
  }
  const inline = `${lines.flatMap((line) => line.id === undefined
    ? [line.source] : [`[id ${line.id}]`, line.source]).join("\n")}\n`;
  const converted = externalizeStoryIds(inline, {path,
    ...(legacyDocumentId === undefined ? {} : {documentId: legacyDocumentId})});
  if (!converted.valid || converted.source === undefined || converted.map === undefined)
    return {valid: false, diagnostics: converted.diagnostics};
  if (Buffer.byteLength(converted.source, "utf8") > 16 * 1024 * 1024)
    return {valid: false, diagnostics: [{severity: "error", code: "PXIMPORT1007",
      message: "Imported Story source exceeds the 16 MiB source limit", path}]};
  if (Buffer.byteLength(canonicalJson(converted.map), "utf8") > 16 * 1024 * 1024)
    return {valid: false, diagnostics: [{severity: "error", code: "PXIMPORT1007",
      message: "Imported Story identity map exceeds the 16 MiB source limit",
      path: `${path}.pxids.json`}]};
  return {valid: true, diagnostics: [],
    value: {source: converted.source, identityMap: converted.map}};
}
