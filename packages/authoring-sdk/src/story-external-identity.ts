import {parseStory} from "./story.js";
import {stableSourceIdReport} from "./story-identity.js";
import type {AuthoringDiagnostic, StoryDocument, StoryNode, StoryNodeKind} from "./types.js";

const identifier = /^[A-Za-z0-9][A-Za-z0-9._-]*$/u;
export const storyIdentityMapMarker = "; @prismatix-ids";

export function storyRequiresIdentityMap(source: string): boolean {
  return source.split(/\r\n|\n/u).some((line) => line.trim() === storyIdentityMapMarker);
}

export interface StoryIdentityMapEntry {
  /** Original `[id ...]` anchor, when the source had one. */
  readonly id?: string;
  /** Exact pre-migration fallback sourceId for an unanchored operation. */
  readonly sourceId?: string;
  readonly kind: StoryNodeKind;
  readonly command?: string;
  /** FNV-1a of the original kind, command and trimmed source line. */
  readonly fingerprint: string;
}

/** Author-only sidecar next to a .pxstory source: `<source>.pxids.json`. */
export interface StoryIdentityMap {
  readonly format: "PrismatiXStoryIdentityMap";
  readonly schemaRevision: 1;
  readonly entries: readonly StoryIdentityMapEntry[];
}

export interface ExternalizeStoryIdsResult {
  readonly source?: string;
  readonly map?: StoryIdentityMap;
  readonly assigned: number;
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}

function hash64(input: string): string {
  let hash = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(input)) {
    hash ^= BigInt(byte);
    hash = BigInt.asUintN(64, hash * 0x100000001b3n);
  }
  return hash.toString(16).padStart(16, "0");
}

function observable(node: StoryNode): boolean {
  if (node.kind === "comment" || node.kind === "speaker") return false;
  return node.kind !== "command" || (node.name !== "id" && node.name !== "choice.wait");
}

function fingerprint(node: StoryNode): string {
  return hash64(`${node.kind}\u0000${node.kind === "command" ? node.name ?? "" : ""}\u0000${node.raw.trim()}`);
}

function issue(code: string, message: string, path: string, node?: StoryNode): AuthoringDiagnostic {
  return {severity: "error", code, message, path,
    ...(node === undefined ? {} : {span: node.span})};
}

/** Move inline `[id ...]` directives into a sidecar, or assign durable ids
 * without modifying a clean Story. Existing authored ids are never changed. */
export function externalizeStoryIds(source: string, options: {
  readonly path?: string; readonly documentId?: string;
} = {}): ExternalizeStoryIdsResult {
  const path = options.path ?? "Story/main.pxstory";
  if (storyRequiresIdentityMap(source)) return {assigned: 0, valid: false,
    diagnostics: [issue("PXSTORY1411",
      "Story already declares an external identity map; restore or inspect its sidecar", path)]};
  const document = parseStory(source, path);
  const report = stableSourceIdReport(document,
    options.documentId === undefined ? {} : {documentId: options.documentId});
  if (!report.valid) return {assigned: 0, diagnostics: report.diagnostics, valid: false};
  const removedLines = new Set(document.nodes.filter((node) =>
    node.kind === "command" && node.name === "id").map((node) => node.span.start.line));
  const newline = source.includes("\r\n") ? "\r\n" : "\n";
  const cleanSource = source.split(/\r\n|\n/u).filter((_, index) =>
    !removedLines.has(index + 1)).join(newline);
  const cleanDocument = parseStory(cleanSource, path);
  const nodes = cleanDocument.nodes.filter(observable);
  if (nodes.length !== report.entries.length || cleanDocument.diagnostics.some((item) => item.severity === "error"))
    return {assigned: 0, diagnostics: [issue("PXSTORY1402",
      "Removing inline ids changed Story operation topology", path), ...cleanDocument.diagnostics], valid: false};

  let assigned = 0;
  const entries = nodes.map((node, index): StoryIdentityMapEntry => {
    const previous = report.entries[index]!;
    if (previous.authoredId === undefined) assigned += 1;
    return {...(previous.authoredId === undefined
      ? {sourceId: previous.sourceId} : {id: previous.authoredId}), kind: node.kind,
      ...(node.kind === "command" ? {command: node.name} : {}),
      fingerprint: fingerprint(node)};
  });
  return {source: `${storyIdentityMapMarker}${newline}${cleanSource}`,
    map: {format: "PrismatiXStoryIdentityMap", schemaRevision: 1, entries},
    assigned, diagnostics: [], valid: true};
}

/** Materialize sidecar ids as non-executable synthetic directives so the
 * existing Story compiler produces byte-for-byte identical operation ids. */
export function applyStoryIdentityMap(document: StoryDocument, value: unknown,
  mapPath = `${document.path}.pxids.json`): StoryDocument {
  const diagnostics: AuthoringDiagnostic[] = [];
  const record = value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown> : undefined;
  const entries = record?.entries;
  if (record?.format !== "PrismatiXStoryIdentityMap" || record.schemaRevision !== 1 ||
      !Array.isArray(entries)) {
    diagnostics.push(issue("PXSTORY1403", "Story identity map has an invalid format", mapPath));
    return {...document, diagnostics: [...document.diagnostics, ...diagnostics]};
  }
  if (document.nodes.some((node) => node.kind === "command" && node.name === "id"))
    diagnostics.push(issue("PXSTORY1404", "Story identity map cannot be combined with inline [id] directives", document.path));
  const nodes = document.nodes.filter(observable);
  if (entries.length !== nodes.length) diagnostics.push(issue("PXSTORY1405",
    `Story identity map has ${entries.length} entries but source has ${nodes.length} operations; review additions/removals before regenerating the map`,
    mapPath));
  const seen = new Set<string>();
  const currentFingerprints = nodes.map(fingerprint);
  const positions = new Map<string, number[]>();
  for (let index = 0; index < currentFingerprints.length; index += 1) {
    const key = currentFingerprints[index]!;
    const existing = positions.get(key) ?? [];
    existing.push(index);
    positions.set(key, existing);
  }
  const anchored = new Map<StoryNode, {id?: string; sourceId?: string}>();
  for (let index = 0; index < entries.length; index += 1) {
    const entry = entries[index];
    const node = nodes[index];
    if (entry === null || typeof entry !== "object" || Array.isArray(entry)) {
      diagnostics.push(issue("PXSTORY1406", `Invalid Story identity entry ${index}`, mapPath));
      continue;
    }
    const anchor = entry as Record<string, unknown>;
    const hasId = typeof anchor.id === "string" && identifier.test(anchor.id);
    const hasSourceId = typeof anchor.sourceId === "string" &&
      /^story-[0-9a-f]{16}$/u.test(anchor.sourceId);
    if (hasId === hasSourceId ||
        typeof anchor.fingerprint !== "string" || !/^[0-9a-f]{16}$/u.test(anchor.fingerprint) ||
        !["label", "dialogue", "narration", "command"].includes(String(anchor.kind)) ||
        (anchor.kind === "command" && typeof anchor.command !== "string")) {
      diagnostics.push(issue("PXSTORY1406", `Invalid Story identity entry ${index}`, mapPath));
      continue;
    }
    const identity = hasId ? `id:${anchor.id}` : `source:${anchor.sourceId}`;
    if (seen.has(identity)) diagnostics.push(issue("PXSTORY1407",
      `Duplicate Story identity map id: ${identity}`, mapPath));
    seen.add(identity);
    if (node === undefined) continue;
    if (node.kind !== anchor.kind || (node.kind === "command" && node.name !== anchor.command)) {
      diagnostics.push(issue("PXSTORY1408",
        `Story operation ${index + 1} changed kind or command; review its saved identity`, document.path, node));
      continue;
    }
    if (currentFingerprints[index] !== anchor.fingerprint &&
        (positions.get(anchor.fingerprint) ?? []).some((position) => position !== index)) {
      diagnostics.push(issue("PXSTORY1409",
        `Story operation ${index + 1} appears to have moved; review its saved identity`, document.path, node));
      continue;
    }
    anchored.set(node, hasId ? {id: anchor.id as string}
      : {sourceId: anchor.sourceId as string});
  }
  if (diagnostics.length > 0) return {...document, diagnostics: [...document.diagnostics, ...diagnostics]};
  const expanded: StoryNode[] = [];
  for (const node of document.nodes) {
    const anchor = anchored.get(node);
    if (anchor?.id !== undefined) expanded.push({id: `story-map-${hash64(`${node.id}:${anchor.id}`)}`,
      kind: "command", name: "id", raw: `[id ${anchor.id}]`, span: node.span,
      arguments: [{raw: anchor.id, value: anchor.id, span: node.span}]});
    expanded.push(anchor?.sourceId === undefined ? node
      : {...node, externalSourceId: anchor.sourceId});
  }
  return {...document, nodes: expanded};
}
