import {createHash} from "node:crypto";

import {serializeDocument, validateDocument} from "./contracts.js";
import type {JsonValue, Result} from "./types.js";

/** An image drawn over an expression layer while the eyes blink or the
 * mouth moves. Coordinates are on the portrait canvas; `x`/`y` default to
 * the owning layer's. */
export interface LayeredPortraitFrame {
  readonly path: string;
  readonly width: number;
  readonly height: number;
  readonly x?: number;
  readonly y?: number;
}

/** A texture layer is placed in the portrait's unscaled canvas coordinates. */
export interface LayeredPortraitLayer {
  readonly path: string;
  readonly width: number;
  readonly height: number;
  readonly x?: number;
  readonly y?: number;
  /** Byte alpha used by the native Stage compositor (0–255). */
  readonly opacity?: number;
  /** Only changed expression layers may crossfade in auto mode. */
  readonly role?: "base" | "expression";
  /** Expression layers only: eye frames from half-closed to closed. The
   * Stage blinks at random every few seconds, playing them in and back. */
  readonly blink?: readonly LayeredPortraitFrame[];
  /** Expression layers only: mouth frames from slightly to fully open,
   * chosen from the loudness of the speaking character's voice. */
  readonly mouth?: readonly LayeredPortraitFrame[];
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
      ...(layer.role === undefined ? {} : {role: layer.role}),
      ...(layer.blink === undefined ? {} : {blink: layer.blink.map(frame)}),
      ...(layer.mouth === undefined ? {} : {mouth: layer.mouth.map(frame)}),
    })),
  };
  return validateDocument<LayeredPortraitDocument>("portrait", document);
}

function frame(input: LayeredPortraitFrame): LayeredPortraitFrame {
  return {path: input.path,
    ...(input.x === undefined ? {} : {x: input.x}),
    ...(input.y === undefined ? {} : {y: input.y}),
    width: input.width, height: input.height};
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
