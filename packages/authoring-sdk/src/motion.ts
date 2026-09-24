import {serializeDocument, validateDocument} from "./contracts.js";
import type {JsonValue, Result} from "./types.js";

export interface MotionLayer {
  readonly id: string;
  readonly image: string;
  /** Pixel rectangles within image; frame tracks select an index without
   * loading a new texture on each key. Requires schema revision 2. */
  readonly frames?: readonly MotionFrame[];
  readonly parent?: string;
  readonly z?: number;
  readonly width: number;
  readonly height: number;
  readonly anchorX?: number;
  readonly anchorY?: number;
  readonly x?: number;
  readonly y?: number;
  readonly scaleX?: number;
  readonly scaleY?: number;
  readonly rotation?: number;
  readonly opacity?: number;
  readonly visible?: boolean;
}

export interface MotionFrame {
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
}

export type MotionProperty = "x" | "y" | "scaleX" | "scaleY" |
  "rotation" | "opacity" | "visible" | "frame";
export type MotionCurve = "step" | "linear" | "easeIn" | "easeOut" | "easeInOut";

export interface MotionKey {
  readonly time: number;
  readonly value: number | boolean;
  readonly curve?: MotionCurve;
}

export interface MotionTrack {
  readonly layer: string;
  readonly property: MotionProperty;
  readonly keys: readonly MotionKey[];
}

export interface MotionInput {
  readonly id: string;
  readonly canvas: {readonly width: number; readonly height: number};
  readonly duration: number;
  readonly loop?: boolean;
  readonly scaleMode?: "fit" | "fill" | "stretch";
  readonly layers: readonly MotionLayer[];
  readonly tracks?: readonly MotionTrack[];
}

export interface MotionDocument extends MotionInput {
  readonly format: "PrismatiXMotion";
  readonly schemaRevision: 1 | 2;
  readonly loop: boolean;
  readonly tracks: readonly MotionTrack[];
}

/** Author a deterministic, image-layer .pxmotion clip without format or
 * revision boilerplate. Atlas frames use revision 2; masks and blur remain
 * unsupported. */
export function defineMotion(input: MotionInput): Result<MotionDocument> {
  const atlas = input.layers.some((layer) => layer.frames !== undefined) ||
    input.tracks?.some((track) => track.property === "frame") === true;
  const document: MotionDocument = {
    format: "PrismatiXMotion",
    schemaRevision: atlas ? 2 : 1,
    id: input.id,
    canvas: input.canvas,
    duration: input.duration,
    loop: input.loop ?? false,
    scaleMode: input.scaleMode ?? "fit",
    layers: input.layers.map((layer) => ({
      id: layer.id,
      image: layer.image,
      ...(layer.frames === undefined ? {} : {frames: layer.frames.map((frame) => ({...frame}))}),
      ...(layer.parent === undefined ? {} : {parent: layer.parent}),
      z: layer.z ?? 0,
      width: layer.width,
      height: layer.height,
      anchorX: layer.anchorX ?? 0,
      anchorY: layer.anchorY ?? 0,
      x: layer.x ?? 0,
      y: layer.y ?? 0,
      scaleX: layer.scaleX ?? 1,
      scaleY: layer.scaleY ?? 1,
      rotation: layer.rotation ?? 0,
      opacity: layer.opacity ?? 1,
      visible: layer.visible ?? true,
    })),
    tracks: (input.tracks ?? []).map((track) => ({
      layer: track.layer,
      property: track.property,
      keys: track.keys.map((key) => ({
        time: key.time,
        value: key.value,
        curve: key.curve ?? (track.property === "visible" || track.property === "frame"
          ? "step" : "linear"),
      })),
    })),
  };
  return validateDocument<MotionDocument>("motion", document);
}

export function serializeMotion(input: MotionInput): Result<string> {
  const defined = defineMotion(input);
  if (!defined.valid || defined.value === undefined)
    return {diagnostics: defined.diagnostics, valid: false};
  return serializeDocument("motion", defined.value as unknown as JsonValue);
}
