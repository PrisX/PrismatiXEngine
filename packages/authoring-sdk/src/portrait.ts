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

/** Returns canonical UTF-8 JSON ready to save as a .pxportrait resource. */
export function serializeLayeredPortrait(input: LayeredPortraitInput): Result<string> {
  const defined = defineLayeredPortrait(input);
  if (!defined.valid || defined.value === undefined)
    return {diagnostics: defined.diagnostics, valid: false};
  return serializeDocument("portrait", defined.value as unknown as JsonValue);
}
