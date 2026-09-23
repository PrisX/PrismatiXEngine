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
}

export interface LayeredPortraitDocument {
  readonly format: "PrismatiXLayeredPortrait";
  readonly schemaRevision: 1;
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
 * format, revision, x/y, or alpha defaults. */
export function defineLayeredPortrait(input: LayeredPortraitInput): Result<LayeredPortraitDocument> {
  const document: LayeredPortraitDocument = {
    format: "PrismatiXLayeredPortrait",
    schemaRevision: 1,
    width: input.width,
    height: input.height,
    layers: input.layers.map((layer) => ({
      path: layer.path,
      x: layer.x ?? 0,
      y: layer.y ?? 0,
      width: layer.width,
      height: layer.height,
      opacity: layer.opacity ?? 255,
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
