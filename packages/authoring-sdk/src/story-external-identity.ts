import {parseStory} from "./story.js";
import {fnv1a64 as hash64} from "./fnv1a64.js";
import {stableSourceIdReport} from "./story-identity.js";
import type {AuthoringDiagnostic, Result, StoryDocument, StoryNode, StoryNodeKind} from "./types.js";

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

export interface SynchronizeStoryIdsResult {
  readonly map?: StoryIdentityMap;
  readonly preserved: number;
  readonly added: number;
  /** New operations requiring a fresh save identity, in current Story order. */
  readonly addedOperations: readonly StoryIdentityAddedOperation[];
  /** Old anchors with no surviving operation; review save migrations. */
  readonly removed: readonly string[];
  /** Removed anchors in their original Story operation order. */
  readonly removedOperations: readonly StoryIdentityRemovedOperation[];
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}

export interface StoryIdentityAddedOperation {
  /** One-based executable-operation index in the edited Story. */
  readonly operation: number;
  /** One-based source line in the edited Story. */
  readonly line: number;
  readonly kind: StoryNodeKind;
  readonly command?: string;
  readonly sourceId: string;
  /** Short source excerpt for human review; never used for identity matching. */
  readonly preview: string;
}

export interface StoryIdentityRemovedOperation {
  /** One-based executable-operation index in the previous sidecar. */
  readonly oldOperation: number;
  readonly identity: string;
  readonly kind: StoryNodeKind;
  readonly command?: string;
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

export function validateStoryIdentityMap(value: unknown,
  mapPath = "Story/main.pxstory.pxids.json"): Result<StoryIdentityMap> {
  const record = value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown> : undefined;
  const entries = record?.entries;
  if (record?.format !== "PrismatiXStoryIdentityMap" || record.schemaRevision !== 1 ||
      !Array.isArray(entries)) return {valid: false,
    diagnostics: [issue("PXSTORY1403", "Story identity map has an invalid format", mapPath)]};
  const diagnostics: AuthoringDiagnostic[] = [];
  const seen = new Set<string>();
  for (let index = 0; index < entries.length; index += 1) {
    const entry = entries[index];
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
  }
  return diagnostics.length > 0 ? {valid: false, diagnostics}
    : {value: value as StoryIdentityMap, diagnostics: [], valid: true};
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

/** Reconcile an edited Story against its existing sidecar without changing
 * identities of operations that can be matched unambiguously. Never writes. */
export function synchronizeStoryIdentityMap(source: string, value: unknown, options: {
  readonly path?: string; readonly mapPath?: string;
} = {}): SynchronizeStoryIdsResult {
  const path = options.path ?? "Story/main.pxstory";
  const mapPath = options.mapPath ?? `${path}.pxids.json`;
  const failed = (diagnostics: readonly AuthoringDiagnostic[]): SynchronizeStoryIdsResult =>
    ({preserved: 0, added: 0, addedOperations: [], removed: [],
      removedOperations: [], diagnostics, valid: false});
  if (!storyRequiresIdentityMap(source)) return failed([issue("PXSTORY1412",
    "Story must declare ; @prismatix-ids before its sidecar can be synchronized", path)]);
  const document = parseStory(source, path);
  if (document.diagnostics.some((diagnostic) => diagnostic.severity === "error"))
    return failed(document.diagnostics);
  if (document.nodes.some((node) => node.kind === "command" && node.name === "id"))
    return failed([issue("PXSTORY1404",
      "Story identity map cannot be combined with inline [id] directives", path)]);
  const validated = validateStoryIdentityMap(value, mapPath);
  if (!validated.valid || validated.value === undefined) return failed(validated.diagnostics);
  const oldMap = validated.value;
  const nodes = document.nodes.filter(observable);
  const current = nodes.map(fingerprint);
  const old = oldMap.entries;
  const sameShape = (oldIndex: number, newIndex: number) =>
    old[oldIndex]!.kind === nodes[newIndex]!.kind &&
    (old[oldIndex]!.kind !== "command" || old[oldIndex]!.command === nodes[newIndex]!.name);
  if (old.length === nodes.length && old.every((entry, index) =>
    sameShape(index, index) && entry.fingerprint === current[index])) {
    return {map: oldMap, preserved: old.length, added: 0,
      addedOperations: [], removed: [], removedOperations: [],
      diagnostics: [], valid: true};
  }

  const oldPositions = new Map<string, number[]>();
  const newPositions = new Map<string, number[]>();
  for (let index = 0; index < old.length; index += 1) {
    const key = old[index]!.fingerprint;
    const positions = oldPositions.get(key) ?? [];
    positions.push(index);
    oldPositions.set(key, positions);
  }
  for (let index = 0; index < nodes.length; index += 1) {
    const key = current[index]!;
    const positions = newPositions.get(key) ?? [];
    positions.push(index);
    newPositions.set(key, positions);
  }
  const anchors: Array<{oldIndex: number; newIndex: number}> = [];
  for (let index = 0; index < old.length; index += 1) {
    const key = old[index]!.fingerprint;
    const matching = newPositions.get(key);
    if (oldPositions.get(key)?.length === 1 && matching?.length === 1)
      anchors.push({oldIndex: index, newIndex: matching[0]!});
  }
  for (let index = 1; index < anchors.length; index += 1) {
    if (anchors[index]!.newIndex <= anchors[index - 1]!.newIndex)
      return failed([issue("PXSTORY1413",
        "Story operations were reordered; review their saved identities manually", path,
        nodes[anchors[index]!.newIndex])]);
  }

  const next: StoryIdentityMapEntry[] = new Array(nodes.length);
  const removed: string[] = [];
  const addedOperations: StoryIdentityAddedOperation[] = [];
  const removedOperations: StoryIdentityRemovedOperation[] = [];
  const identities = new Set(old.map((entry) => entry.id === undefined
    ? `source:${entry.sourceId}` : `id:${entry.id}`));
  let preserved = 0;
  let added = 0;
  const keep = (oldIndex: number, newIndex: number) => {
    next[newIndex] = {...old[oldIndex]!, fingerprint: current[newIndex]!};
    preserved += 1;
  };
  const reconcile = (oldStart: number, oldEnd: number,
    newStart: number, newEnd: number): AuthoringDiagnostic | undefined => {
    while (oldStart < oldEnd && newStart < newEnd &&
        old[oldStart]!.fingerprint === current[newStart] && sameShape(oldStart, newStart)) {
      keep(oldStart, newStart);
      oldStart += 1;
      newStart += 1;
    }
    while (oldStart < oldEnd && newStart < newEnd &&
        old[oldEnd - 1]!.fingerprint === current[newEnd - 1] &&
        sameShape(oldEnd - 1, newEnd - 1)) {
      keep(oldEnd - 1, newEnd - 1);
      oldEnd -= 1;
      newEnd -= 1;
    }
    const oldCount = oldEnd - oldStart;
    const newCount = newEnd - newStart;
    if (oldCount > 0 && newCount > 0 && oldCount !== newCount)
      return issue("PXSTORY1414",
        "Story edits are ambiguous: changed and inserted/removed operations overlap. If possible, sync the prose rewrite first, then add/remove lines and sync again; otherwise review old save identities manually",
        path, nodes[newStart]);
    if (oldCount > 0 && newCount > 0) {
      for (let offset = 0; offset < oldCount; offset += 1) {
        const oldIndex = oldStart + offset;
        const newIndex = newStart + offset;
        if (!sameShape(oldIndex, newIndex)) return issue("PXSTORY1415",
          "Story operation changed kind or command; review its saved identity manually",
          path, nodes[newIndex]);
        if ((newPositions.get(old[oldIndex]!.fingerprint) ?? []).some((position) =>
          position !== newIndex)) return issue("PXSTORY1413",
          "Story operation appears to have moved; review its saved identity manually",
          path, nodes[newIndex]);
        keep(oldIndex, newIndex);
      }
    } else if (newCount > 0) {
      for (let index = newStart; index < newEnd; index += 1) {
        if (oldPositions.has(current[index]!)) return issue("PXSTORY1416",
          "Inserted Story operation duplicates an existing operation; review identities manually",
          path, nodes[index]);
        let salt = 0;
        let identity: string;
        do {
          identity = `story-${hash64(`external-new:${path}:${nodes[index]!.id}:${index}:${salt}`)}`;
          salt += 1;
        } while (identities.has(`source:${identity}`));
        identities.add(`source:${identity}`);
        const node = nodes[index]!;
        next[index] = {sourceId: identity, kind: node.kind,
          ...(node.kind === "command" ? {command: node.name} : {}),
          fingerprint: current[index]!};
        addedOperations.push({operation: index + 1, line: node.span.start.line,
          kind: node.kind,
          ...(node.kind === "command" ? {command: node.name} : {}),
          sourceId: identity, preview: Array.from(node.raw.trim()).slice(0, 120).join("")});
        added += 1;
      }
    } else {
      for (let index = oldStart; index < oldEnd; index += 1) {
        const entry = old[index]!;
        if (newPositions.has(entry.fingerprint)) return issue("PXSTORY1413",
          "Removed Story operation appears elsewhere; review its saved identity manually", path);
        const identity = entry.id === undefined ? `source:${entry.sourceId}` : `id:${entry.id}`;
        removed.push(identity);
        removedOperations.push({oldOperation: index + 1, identity,
          kind: entry.kind,
          ...(entry.kind === "command" ? {command: entry.command} : {})});
      }
    }
    return undefined;
  };

  let oldStart = 0;
  let newStart = 0;
  for (const anchor of [...anchors, {oldIndex: old.length, newIndex: nodes.length}]) {
    const diagnostic = reconcile(oldStart, anchor.oldIndex, newStart, anchor.newIndex);
    if (diagnostic !== undefined) return failed([diagnostic]);
    if (anchor.oldIndex < old.length) {
      keep(anchor.oldIndex, anchor.newIndex);
      oldStart = anchor.oldIndex + 1;
      newStart = anchor.newIndex + 1;
    }
  }
  const map: StoryIdentityMap = {format: "PrismatiXStoryIdentityMap",
    schemaRevision: 1, entries: next};
  const applied = applyStoryIdentityMap(document, map, mapPath);
  if (applied.diagnostics.some((diagnostic) => diagnostic.severity === "error"))
    return failed(applied.diagnostics);
  return {map, preserved, added, addedOperations, removed,
    removedOperations, diagnostics: [], valid: true};
}

/** Preserve sidecar identities when an importer can prove that every edited
 * executable line is the same operation in the same position. The caller's
 * predicate must be narrow: it is the proof for changed lines, not a general
 * permission to ignore reordering. Insertions and deletions always fail. */
export function synchronizeAlignedStoryIdentityMap(
  previousSource: string, source: string, value: unknown, options: {
    readonly path?: string;
    readonly mapPath?: string;
    readonly isAllowedEdit: (previousRaw: string, currentRaw: string,
      kind: StoryNodeKind, command?: string) => boolean;
  }): SynchronizeStoryIdsResult {
  const path = options.path ?? "Story/main.pxstory";
  const mapPath = options.mapPath ?? `${path}.pxids.json`;
  const failed = (diagnostics: readonly AuthoringDiagnostic[]): SynchronizeStoryIdsResult =>
    ({preserved: 0, added: 0, addedOperations: [], removed: [],
      removedOperations: [], diagnostics, valid: false});
  if (!storyRequiresIdentityMap(previousSource) || !storyRequiresIdentityMap(source))
    return failed([issue("PXSTORY1412", "Both Story sources must declare an external identity map", path)]);
  const previous = parseStory(previousSource, path);
  const current = parseStory(source, path);
  const parseErrors = [...previous.diagnostics, ...current.diagnostics]
    .filter((diagnostic) => diagnostic.severity === "error");
  if (parseErrors.length > 0) return failed(parseErrors);
  if ([...previous.nodes, ...current.nodes].some((node) =>
    node.kind === "command" && node.name === "id"))
    return failed([issue("PXSTORY1404", "Story identity map cannot be combined with inline [id] directives", path)]);
  const validated = validateStoryIdentityMap(value, mapPath);
  if (!validated.valid || validated.value === undefined) return failed(validated.diagnostics);
  const before = previous.nodes.filter(observable);
  const after = current.nodes.filter(observable);
  const old = validated.value.entries;
  if (old.length !== before.length || before.length !== after.length)
    return failed([issue("PXSTORY1414", "Aligned Story synchronization cannot add or remove executable operations", path)]);
  const entries: StoryIdentityMapEntry[] = [];
  for (let index = 0; index < after.length; index += 1) {
    const original = before[index]!;
    const edited = after[index]!;
    const entry = old[index]!;
    if (entry.fingerprint !== fingerprint(original) ||
        entry.kind !== original.kind || entry.kind !== edited.kind ||
        (entry.kind === "command" &&
          (entry.command !== original.name || entry.command !== edited.name)))
      return failed([issue("PXSTORY1415", "Existing Story identity map or operation shape is not aligned", path, edited)]);
    if (original.raw.trim() !== edited.raw.trim() &&
        !options.isAllowedEdit(original.raw.trim(), edited.raw.trim(),
          edited.kind, edited.kind === "command" ? edited.name : undefined))
      return failed([issue("PXSTORY1417", "Story operation edit was not approved for aligned synchronization", path, edited)]);
    entries.push({...entry, fingerprint: fingerprint(edited)});
  }
  const map: StoryIdentityMap = {...validated.value, entries};
  const applied = applyStoryIdentityMap(current, map, mapPath);
  if (applied.diagnostics.some((diagnostic) => diagnostic.severity === "error"))
    return failed(applied.diagnostics);
  return {map, preserved: entries.length, added: 0, addedOperations: [],
    removed: [], removedOperations: [], diagnostics: [], valid: true};
}

/** Materialize sidecar ids as non-executable synthetic directives so the
 * existing Story compiler produces byte-for-byte identical operation ids. */
export function applyStoryIdentityMap(document: StoryDocument, value: unknown,
  mapPath = `${document.path}.pxids.json`): StoryDocument {
  const diagnostics: AuthoringDiagnostic[] = [];
  const validated = validateStoryIdentityMap(value, mapPath);
  if (!validated.valid || validated.value === undefined)
    return {...document, diagnostics: [...document.diagnostics, ...validated.diagnostics]};
  const entries = validated.value.entries;
  if (document.nodes.some((node) => node.kind === "command" && node.name === "id"))
    diagnostics.push(issue("PXSTORY1404", "Story identity map cannot be combined with inline [id] directives", document.path));
  const nodes = document.nodes.filter(observable);
  if (entries.length !== nodes.length) diagnostics.push(issue("PXSTORY1405",
    `Story identity map has ${entries.length} entries but source has ${nodes.length} operations; run prismatix ids <project> --sync to review additions/removals, then --sync --write to update the map`,
    mapPath));
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
    const anchor = entries[index]!;
    const node = nodes[index];
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
    anchored.set(node, anchor.id === undefined ? {sourceId: anchor.sourceId!}
      : {id: anchor.id});
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
