import {validateDocument} from "./contracts.js";
import type {SaveMigrationAnchor, SaveMigrationAnchorPolicy} from "./project.js";
import type {AuthoringDiagnostic, Result, RuntimeIrDocument, RuntimeIrOperation} from "./types.js";

/** A reviewed replacement for one operation in the previous release. */
export interface SaveAnchorReplacement {
  readonly fromOperationId: string;
  readonly toOperationId: string;
}

export interface SaveAnchorPolicyPlan {
  readonly anchor: SaveMigrationAnchorPolicy;
  readonly preserved: number;
  readonly mapped: number;
}

function issue(code: string, message: string, operation?: RuntimeIrOperation): AuthoringDiagnostic {
  return {severity: "error", code, message,
    ...(operation === undefined ? {} : {sourceId: operation.sourceId,
      details: `operationId=${operation.operationId}`})};
}

function anchor(documentId: string, operation: RuntimeIrOperation): SaveMigrationAnchor {
  return {runtimeDocumentId: documentId,
    sourceId: operation.sourceId, operationId: operation.operationId};
}

function anchorKey(value: SaveMigrationAnchor): string {
  return JSON.stringify([value.runtimeDocumentId, value.sourceId, value.operationId]);
}

/**
 * Review old and new compiled Runtime IR before writing a sparse save migration.
 * Every old anchor must survive exactly or have an explicit replacement. This
 * checks identities, not whether the replacement has the same narrative meaning.
 */
export function planSaveAnchorPolicy(previous: RuntimeIrDocument,
  next: RuntimeIrDocument,
  replacements: readonly SaveAnchorReplacement[] = []): Result<SaveAnchorPolicyPlan> {
  const oldValidation = validateDocument<RuntimeIrDocument>("runtimeIr", previous);
  const newValidation = validateDocument<RuntimeIrDocument>("runtimeIr", next);
  if (!oldValidation.valid || !newValidation.valid) return {valid: false,
    diagnostics: [...oldValidation.diagnostics, ...newValidation.diagnostics]};

  const diagnostics: AuthoringDiagnostic[] = [];
  const oldById = new Map<string, RuntimeIrOperation>();
  const newById = new Map<string, RuntimeIrOperation>();
  for (const [document, index, label] of [
    [previous, oldById, "previous"], [next, newById, "next"],
  ] as const) for (const operation of document.operations) {
    if (index.has(operation.operationId))
      diagnostics.push(issue("PXSDKMIG2001", `Duplicate operation ID in ${label} Runtime IR`, operation));
    else index.set(operation.operationId, operation);
  }
  if (diagnostics.length > 0) return {valid: false, diagnostics};

  const requested = new Map<string, RuntimeIrOperation>();
  for (const replacement of replacements) {
    const from = oldById.get(replacement.fromOperationId);
    const to = newById.get(replacement.toOperationId);
    if (!from || !to) {
      diagnostics.push(issue("PXSDKMIG2002",
        `Replacement refers to an unknown ${!from ? "previous" : "next"} operation ID`));
      continue;
    }
    if (requested.has(from.operationId)) {
      diagnostics.push(issue("PXSDKMIG2003", "Previous operation has more than one replacement", from));
      continue;
    }
    requested.set(from.operationId, to);
  }
  if (diagnostics.length > 0) return {valid: false, diagnostics};

  const surviving = new Set(next.operations.map((operation) =>
    anchorKey(anchor(next.documentId, operation))));
  const mappings: {from: SaveMigrationAnchor; to: SaveMigrationAnchor}[] = [];
  let preserved = 0;
  let missing = 0;
  for (const operation of previous.operations) {
    const from = anchor(previous.documentId, operation);
    const replacement = requested.get(operation.operationId);
    if (replacement) {
      mappings.push({from, to: anchor(next.documentId, replacement)});
    } else if (surviving.has(anchorKey(from))) {
      preserved += 1;
    } else {
      missing += 1;
      if (missing <= 20) diagnostics.push(issue("PXSDKMIG2004",
        "Old save anchor is absent from the new Runtime IR; choose a replacement", operation));
    }
  }
  if (missing > 20) diagnostics.push(issue("PXSDKMIG2004",
    `${missing - 20} additional old save anchors need replacements`));
  if (mappings.length > 100_000) diagnostics.push(issue("PXSDKMIG2005",
    "Anchor mappings exceed the native 100,000-entry limit"));
  if (diagnostics.length > 0) return {valid: false, diagnostics};

  return {valid: true, diagnostics: [], value: {
    anchor: mappings.length === 0 ? {policy: "preserve"} :
      {policy: "map-or-preserve", mappings},
    preserved, mapped: mappings.length,
  }};
}
