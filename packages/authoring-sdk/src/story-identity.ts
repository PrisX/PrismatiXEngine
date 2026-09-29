import {parseStory} from "./story.js";
import {fnv1a64 as hash64} from "./fnv1a64.js";
import type {AuthoringDiagnostic, StoryDocument, StoryNode} from "./types.js";

const identifierPattern = /^[A-Za-z0-9][A-Za-z0-9._-]*$/u;

export interface StableSourceIdEntry {
  readonly sourceId: string;
  readonly authoredId?: string;
  readonly stable: boolean;
  readonly kind: string;
  readonly command?: string;
  readonly path: string;
  readonly line: number;
  readonly column: number;
}

export interface StableSourceIdReport {
  readonly path: string;
  readonly documentId: string;
  readonly entries: readonly StableSourceIdEntry[];
  readonly total: number;
  readonly anchored: number;
  readonly fallback: number;
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}

export interface StableSourceIdReportOptions {
  readonly path?: string;
  readonly documentId?: string;
}

export interface AssignStableSourceIdsOptions extends StableSourceIdReportOptions {
  /** Prefix for generated authored ids. Defaults to `auto`. */
  readonly prefix?: string;
}

export interface StableSourceIdAssignmentResult {
  readonly source?: string;
  readonly assigned: number;
  readonly report: StableSourceIdReport;
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}

interface ScannedEntry {
  readonly node: StoryNode;
  readonly authoredId?: string;
  readonly sourceId: string;
  readonly stable: boolean;
}

function diagnostic(code: string, message: string, node?: StoryNode, details?: string): AuthoringDiagnostic {
  return {
    severity: "error",
    code,
    message,
    ...(node === undefined ? {} : {path: node.span.path, span: node.span}),
    ...(details === undefined ? {} : {details}),
  };
}

function authoredId(node: StoryNode): string | undefined {
  if (node.kind !== "command" || node.name !== "id") return undefined;
  const argument = node.arguments?.find((value) => value.name === "value") ??
    node.arguments?.find((value) => value.name === undefined);
  return typeof argument?.value === "string" ? argument.value : undefined;
}

function observable(node: StoryNode): boolean {
  if (node.kind === "comment" || node.kind === "speaker") return false;
  // A [macro] definition emits nothing itself; its calls are the operations.
  return node.kind !== "command" ||
    (node.name !== "id" && node.name !== "choice.wait" && node.name !== "macro");
}

function scan(document: StoryDocument, documentId: string): {
  entries: ScannedEntry[];
  diagnostics: AuthoringDiagnostic[];
} {
  const diagnostics = [...document.diagnostics];
  const entries: ScannedEntry[] = [];
  const seen = new Set<string>();
  let pending: {id: string; node: StoryNode} | undefined;
  for (const node of document.nodes) {
    const identity = authoredId(node);
    if (node.kind === "command" && node.name === "id") {
      if (identity === undefined || !identifierPattern.test(identity)) {
        diagnostics.push(diagnostic("PXSTORY1210", "id requires a stable identifier", node));
      } else if (pending !== undefined) {
        diagnostics.push(diagnostic("PXSTORY1211",
          "id must be followed by one observable Story operation", node));
      } else if (seen.has(identity)) {
        diagnostics.push(diagnostic("PXSTORY1212", `Duplicate authored Story id: ${identity}`, node));
      } else {
        seen.add(identity);
        pending = {id: identity, node};
      }
      continue;
    }
    if (!observable(node)) continue;
    const pinned = pending?.id;
    pending = undefined;
    entries.push({
      node,
      ...(pinned === undefined ? {} : {authoredId: pinned}),
      sourceId: pinned === undefined ? node.externalSourceId ?? node.id
        : `story-${hash64(`${documentId}:${pinned}`)}`,
      stable: pinned !== undefined || node.externalSourceId !== undefined,
    });
  }
  if (pending !== undefined) {
    diagnostics.push(diagnostic("PXSTORY1213",
      `Story id ${pending.id} is not followed by an observable operation`,
      pending.node));
  }
  return {entries, diagnostics};
}

/** Reports which observable Story operations have durable authored anchors and
 * which still rely on the path/offset/prose fallback identity. */
export function stableSourceIdReport(
  source: string | StoryDocument,
  options: StableSourceIdReportOptions = {},
): StableSourceIdReport {
  const path = typeof source === "string" ? options.path ?? "Story/main.pxstory" : source.path;
  const document = typeof source === "string" ? parseStory(source, path) : source;
  const documentId = options.documentId ?? "main";
  const scanned = scan(document, documentId);
  const entries = scanned.entries.map(({node, authoredId: id, sourceId, stable}) => ({
    sourceId,
    ...(id === undefined ? {} : {authoredId: id}),
    stable,
    kind: node.kind,
    ...(node.kind === "command" && node.name !== undefined ? {command: node.name} : {}),
    path: node.span.path,
    line: node.span.start.line,
    column: node.span.start.column,
  }));
  const anchored = entries.filter((entry) => entry.stable).length;
  return {
    path: document.path,
    documentId,
    entries,
    total: entries.length,
    anchored,
    fallback: entries.length - anchored,
    diagnostics: scanned.diagnostics,
    valid: !scanned.diagnostics.some((item) => item.severity === "error"),
  };
}

/** Inserts an `[id ...]` directive before every unanchored observable Story
 * operation. This is deliberately an explicit source rewrite: after the first
 * run, prose edits, inserted lines, and source-file renames preserve identity. */
export function assignStableSourceIds(
  source: string,
  options: AssignStableSourceIdsOptions = {},
): StableSourceIdAssignmentResult {
  const path = options.path ?? "Story/main.pxstory";
  const documentId = options.documentId ?? "main";
  const prefix = options.prefix ?? "auto";
  const before = parseStory(source, path);
  const scanned = scan(before, documentId);
  if (!identifierPattern.test(prefix)) {
    const badPrefix = diagnostic("PXSTORY1214",
      "Stable Story id prefix must be an identifier", undefined, prefix);
    const report = stableSourceIdReport(before, {documentId});
    return {assigned: 0, report, diagnostics: [...scanned.diagnostics, badPrefix], valid: false};
  }
  if (scanned.diagnostics.some((item) => item.severity === "error")) {
    const report = stableSourceIdReport(before, {documentId});
    return {assigned: 0, report, diagnostics: scanned.diagnostics, valid: false};
  }

  const existing = new Set(scanned.entries
    .map((entry) => entry.authoredId)
    .filter((value): value is string => value !== undefined));
  const insertions = new Map<number, string>();
  let ordinal = 0;
  for (const entry of scanned.entries) {
    if (entry.authoredId !== undefined) continue;
    let candidate = `${prefix}.${hash64(`${path}:${entry.node.id}:${ordinal}`)}`;
    for (let collision = 1; existing.has(candidate); collision += 1)
      candidate = `${prefix}.${hash64(`${path}:${entry.node.id}:${ordinal}:${collision}`)}`;
    existing.add(candidate);
    const indentation = entry.node.raw.match(/^\s*/u)?.[0] ?? "";
    insertions.set(entry.node.span.start.line, `${indentation}[id ${candidate}]`);
    ordinal += 1;
  }

  const newline = source.includes("\r\n") ? "\r\n" : "\n";
  const lines = source.split(/\r\n|\n/u);
  const output: string[] = [];
  for (let index = 0; index < lines.length; index += 1) {
    const insertion = insertions.get(index + 1);
    if (insertion !== undefined) output.push(insertion);
    output.push(lines[index]!);
  }
  const rewritten = output.join(newline);
  const report = stableSourceIdReport(rewritten, {path, documentId});
  return {
    source: rewritten,
    assigned: insertions.size,
    report,
    diagnostics: report.diagnostics,
    valid: report.valid,
  };
}
