// motion3.json and exp3.json: parsing, curve evaluation and fading. This is
// the small subset of the Cubism Framework a visual novel needs (parameter and
// part-opacity curves, Bezier/linear/stepped segments, fades, expressions);
// physics, pose switching and user-data events are not implemented.

import type {ParameterBank} from "./parameters.ts";

const TWO_PI = Math.PI * 2;

interface Segment {
  /** 0 linear, 1 Bezier, 2 stepped, 3 inverse stepped */
  readonly type: number;
  readonly t0: number;
  readonly v0: number;
  readonly t1: number;
  readonly v1: number;
  readonly c1t: number;
  readonly c1v: number;
  readonly c2t: number;
  readonly c2v: number;
}

export interface Curve {
  readonly target: string;
  readonly id: string;
  readonly fadeIn: number | undefined;
  readonly fadeOut: number | undefined;
  readonly segments: readonly Segment[];
}

export interface Motion {
  readonly duration: number;
  readonly loop: boolean;
  readonly fadeIn: number | undefined;
  readonly fadeOut: number | undefined;
  readonly curves: readonly Curve[];
}

interface RawMotion {
  Meta?: {Duration?: number; Loop?: boolean; FadeInTime?: number; FadeOutTime?: number};
  Curves?: {Target: string; Id: string; FadeInTime?: number; FadeOutTime?: number; Segments: number[]}[];
}

function parseSegments(raw: readonly number[]): Segment[] {
  const segments: Segment[] = [];
  if (raw.length < 2) return segments;
  let t0 = raw[0]!, v0 = raw[1]!;
  for (let at = 2; at < raw.length;) {
    const type = raw[at]!;
    if (type === 1) {
      const t1 = raw[at + 5]!, v1 = raw[at + 6]!;
      segments.push({type, t0, v0, t1, v1, c1t: raw[at + 1]!, c1v: raw[at + 2]!, c2t: raw[at + 3]!, c2v: raw[at + 4]!});
      t0 = t1; v0 = v1; at += 7;
    } else if (type === 0 || type === 2 || type === 3) {
      const t1 = raw[at + 1]!, v1 = raw[at + 2]!;
      segments.push({type, t0, v0, t1, v1, c1t: 0, c1v: 0, c2t: 0, c2v: 0});
      t0 = t1; v0 = v1; at += 3;
    } else {
      throw new Error(`Live2D: unknown motion segment type ${type}`);
    }
  }
  return segments;
}

export function parseMotion(text: string): Motion {
  const json = JSON.parse(text) as RawMotion;
  return {
    duration: json.Meta?.Duration ?? 0,
    loop: json.Meta?.Loop ?? false,
    fadeIn: json.Meta?.FadeInTime,
    fadeOut: json.Meta?.FadeOutTime,
    curves: (json.Curves ?? []).map((curve) => ({
      target: curve.Target, id: curve.Id, fadeIn: curve.FadeInTime, fadeOut: curve.FadeOutTime,
      segments: parseSegments(curve.Segments),
    })),
  };
}

function cubic(p0: number, p1: number, p2: number, p3: number, u: number): number {
  const v = 1 - u;
  return v * v * v * p0 + 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u * p3;
}

/** The curve's value at `time` seconds. */
export function evaluateCurve(curve: Curve, time: number): number {
  const segments = curve.segments;
  if (segments.length === 0) return 0;
  const first = segments[0]!;
  if (time <= first.t0) return first.v0;
  let low = 0, high = segments.length - 1;
  while (low < high) {
    const middle = (low + high + 1) >> 1;
    if (segments[middle]!.t0 <= time) low = middle; else high = middle - 1;
  }
  const segment = segments[low]!;
  if (time >= segment.t1) return segment.v1;
  switch (segment.type) {
    case 0: return segment.v0 + ((segment.v1 - segment.v0) * (time - segment.t0)) / (segment.t1 - segment.t0);
    case 2: return segment.v0;
    case 3: return segment.v1;
    default: {
      // Solve x(u) = time on the curve's time axis, then read y(u).
      let lo = 0, hi = 1;
      for (let step = 0; step < 24; step += 1) {
        const mid = (lo + hi) / 2;
        if (cubic(segment.t0, segment.c1t, segment.c2t, segment.t1, mid) < time) lo = mid; else hi = mid;
      }
      return cubic(segment.v0, segment.c1v, segment.c2v, segment.v1, (lo + hi) / 2);
    }
  }
}

function easeSine(x: number): number {
  return x <= 0 ? 0 : x >= 1 ? 1 : 0.5 - 0.5 * Math.cos(x * Math.PI);
}

/** Plays a motion: `advance` moves its clock, `apply` blends its curves into the parameters. */
export class MotionPlayer {
  private time = 0;
  readonly motion: Motion;
  readonly loop: boolean;
  readonly fadeIn: number;
  readonly fadeOut: number;
  private readonly controlled = new Set<string>();

  constructor(motion: Motion, loop: boolean, fadeIn?: number, fadeOut?: number) {
    this.motion = motion;
    this.loop = loop;
    // Cubism's default fade is one second when nothing says otherwise.
    this.fadeIn = fadeIn ?? motion.fadeIn ?? 1;
    this.fadeOut = fadeOut ?? motion.fadeOut ?? 1;
    for (const curve of motion.curves) this.controlled.add(curve.id);
  }

  get finished(): boolean {
    return !this.loop && this.time >= this.motion.duration + this.fadeOut;
  }

  /** True when a curve of this motion writes the parameter (so blink must leave it alone). */
  controls(id: string): boolean {
    return this.controlled.has(id);
  }

  advance(deltaSeconds: number): void {
    this.time += deltaSeconds;
    if (this.loop && this.motion.duration > 0 && this.time >= this.motion.duration) this.time %= this.motion.duration;
  }

  /** Motion weight at the current time; `stopped` is how long ago it was told to end (or -1). */
  private weight(fadeIn: number, fadeOut: number): number {
    const inWeight = fadeIn === 0 ? 1 : easeSine(this.time / fadeIn);
    const outWeight = this.loop || fadeOut === 0 ? 1 : easeSine((this.motion.duration - this.time) / fadeOut);
    return inWeight * outWeight;
  }

  apply(parameters: ParameterBank, overall: number): void {
    for (const curve of this.motion.curves) {
      const weight = this.weight(curve.fadeIn ?? this.fadeIn, curve.fadeOut ?? this.fadeOut) * overall;
      const value = evaluateCurve(curve, Math.min(this.time, this.motion.duration));
      if (curve.target === "Parameter") parameters.blendToward(curve.id, value, weight);
      else if (curve.target === "PartOpacity") parameters.blendPart(curve.id, value, weight);
    }
  }
}

export type ExpressionBlend = "Add" | "Multiply" | "Overwrite";

export interface Expression {
  readonly fadeIn: number;
  readonly fadeOut: number;
  readonly parameters: readonly {readonly id: string; readonly value: number; readonly blend: ExpressionBlend}[];
}

export function parseExpression(text: string): Expression {
  const json = JSON.parse(text) as {FadeInTime?: number; FadeOutTime?: number; Parameters?: {Id: string; Value: number; Blend?: string}[]};
  return {
    fadeIn: json.FadeInTime ?? 1,
    fadeOut: json.FadeOutTime ?? 1,
    parameters: (json.Parameters ?? []).map((entry) => ({
      id: entry.Id, value: entry.Value,
      blend: entry.Blend === "Multiply" ? "Multiply" : entry.Blend === "Overwrite" ? "Overwrite" : "Add",
    })),
  };
}

/** Eases an expression in over its fade time and applies it on top of whatever the motion wrote. */
export class ExpressionPlayer {
  private time = 0;
  private readonly expression: Expression;

  constructor(expression: Expression) {
    this.expression = expression;
  }

  advance(deltaSeconds: number): void {
    this.time += deltaSeconds;
  }

  apply(parameters: ParameterBank, overall: number): void {
    const weight = (this.expression.fadeIn === 0 ? 1 : easeSine(this.time / this.expression.fadeIn)) * overall;
    for (const entry of this.expression.parameters) {
      const current = parameters.get(entry.id);
      if (current === undefined) continue;
      if (entry.blend === "Add") parameters.set(entry.id, current + entry.value * weight);
      else if (entry.blend === "Multiply") parameters.set(entry.id, current * (1 + (entry.value - 1) * weight));
      else parameters.set(entry.id, current * (1 - weight) + entry.value * weight);
    }
  }
}

/** Cubism's breath: a few sines added to head, body and ParamBreath. */
const BREATH: readonly {id: string; offset: number; peak: number; cycle: number}[] = [
  {id: "ParamAngleX", offset: 0, peak: 15, cycle: 6.5345},
  {id: "ParamAngleY", offset: 0, peak: 8, cycle: 3.5345},
  {id: "ParamAngleZ", offset: 0, peak: 10, cycle: 5.5345},
  {id: "ParamBodyAngleX", offset: 0, peak: 4, cycle: 15.5345},
  {id: "ParamBreath", offset: 0.5, peak: 0.5, cycle: 3.2345},
];

export function applyBreath(parameters: ParameterBank, seconds: number): void {
  for (const entry of BREATH) {
    const current = parameters.get(entry.id);
    if (current !== undefined) parameters.set(entry.id, current + entry.offset + entry.peak * Math.sin((seconds * TWO_PI) / entry.cycle));
  }
}

/**
 * Blinks the model3.json's EyeBlink parameters at irregular intervals. The
 * sandbox has no Math.random (and a saved game must replay), so the intervals
 * come from a small seeded generator.
 */
export class Blinker {
  private state = 0;
  private clock = 0;
  private next: number;
  private seed: number;
  private readonly ids: readonly string[];
  private static readonly CLOSING = 0.1;
  private static readonly CLOSED = 0.05;
  private static readonly OPENING = 0.15;

  constructor(ids: readonly string[], seed: number) {
    this.ids = ids;
    this.seed = seed;
    this.next = this.interval();
  }

  private random(): number {
    this.seed = (Math.imul(this.seed, 1664525) + 1013904223) >>> 0;
    return this.seed / 4294967296;
  }

  private interval(): number {
    return 1 + this.random() * 5;
  }

  advance(deltaSeconds: number): void {
    this.clock += deltaSeconds;
    if (this.state === 0 && this.clock >= this.next) { this.state = 1; this.clock = 0; }
    else if (this.state === 1 && this.clock >= Blinker.CLOSING) { this.state = 2; this.clock = 0; }
    else if (this.state === 2 && this.clock >= Blinker.CLOSED) { this.state = 3; this.clock = 0; }
    else if (this.state === 3 && this.clock >= Blinker.OPENING) { this.state = 0; this.clock = 0; this.next = this.interval(); }
  }

  /** 1 open ... 0 closed. */
  get openness(): number {
    switch (this.state) {
      case 1: return 1 - this.clock / Blinker.CLOSING;
      case 2: return 0;
      case 3: return this.clock / Blinker.OPENING;
      default: return 1;
    }
  }

  apply(parameters: ParameterBank, skip: (id: string) => boolean): void {
    if (this.state === 0) return;
    for (const id of this.ids) if (!skip(id)) parameters.set(id, this.openness);
  }
}
