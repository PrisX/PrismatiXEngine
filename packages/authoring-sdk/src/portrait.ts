import {createHash} from "node:crypto";

import {serializeDocument, validateDocument} from "./contracts.js";
import type {JsonValue, Result} from "./types.js";

/** A texture layer is placed in the portrait's unscaled canvas coordinates. */
export interface LayeredPortraitLayer {
  readonly path: string;
  readonly width: number;
  readonly height: number;
  readonly x?: number;
  readonly y?: number;
  /** Byte alpha used by the native Stage compositor (0–255). */
  readonly opacity?: number;
  /** Revision 2: only changed expression layers may crossfade in auto mode. */
  readonly role?: "base" | "expression";
}

export interface LayeredPortraitDocument {
  readonly format: "PrismatiXLayeredPortrait";
  readonly schemaRevision: 1 | 2;
  readonly width: number;
  readonly height: number;
  readonly layers: readonly LayeredPortraitLayer[];
}

export interface LayeredPortraitInput {
  readonly width: number;
  readonly height: number;
  readonly layers: readonly LayeredPortraitLayer[];
}

/** Creates and validates the public .pxportrait document without hand-writing
 * format, revision, x/y, or alpha defaults. All layers need a role when any
 * one uses it; legacy untagged portraits remain revision 1. */
export function defineLayeredPortrait(input: LayeredPortraitInput): Result<LayeredPortraitDocument> {
  const hasRoles = input.layers.some((layer) => layer.role !== undefined);
  const document: LayeredPortraitDocument = {
    format: "PrismatiXLayeredPortrait",
    schemaRevision: hasRoles ? 2 : 1,
    width: input.width,
    height: input.height,
    layers: input.layers.map((layer) => ({
      path: layer.path,
      x: layer.x ?? 0,
      y: layer.y ?? 0,
      width: layer.width,
      height: layer.height,
      opacity: layer.opacity ?? 255,
      ...(layer.role === undefined ? {} : {role: layer.role}),
    })),
  };
  return validateDocument<LayeredPortraitDocument>("portrait", document);
}

/**
 * Layered portraits the Story compiler may read, keyed by project path. The
 * `[face]` command uses it to find, for each body, the expression that shows
 * a given face; `fingerprint` lets build caches notice a changed portrait.
 */
export interface StoryPortraitIndex {
  readonly fingerprint: string;
  layers(source: string): LayeredPortraitDocument | undefined;
}

export function createStoryPortraitIndex(
  portraits: Readonly<Record<string, LayeredPortraitDocument>>,
): StoryPortraitIndex {
  const entries = new Map(Object.entries(portraits).sort(([left], [right]) =>
    left < right ? -1 : left > right ? 1 : 0));
  const fingerprint = createHash("sha256")
    .update(JSON.stringify([...entries])).digest("hex");
  return {fingerprint, layers: (source) => entries.get(source)};
}

/** Returns canonical UTF-8 JSON ready to save as a .pxportrait resource. */
export function serializeLayeredPortrait(input: LayeredPortraitInput): Result<string> {
  const defined = defineLayeredPortrait(input);
  if (!defined.valid || defined.value === undefined)
    return {diagnostics: defined.diagnostics, valid: false};
  return serializeDocument("portrait", defined.value as unknown as JsonValue);
}
