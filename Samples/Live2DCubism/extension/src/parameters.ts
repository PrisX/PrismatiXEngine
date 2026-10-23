import type {ParameterInfo} from "./protocol.ts";

/**
 * The model's parameters and part opacities for one frame. Each frame starts
 * from the defaults and the motion, expression, blink and the script's own
 * overrides write on top, so nothing carries over by accident.
 */
export class ParameterBank {
  readonly values: Float32Array;
  readonly partValues: Float32Array;
  private readonly index = new Map<string, number>();
  private readonly partIndex = new Map<string, number>();
  private readonly info: readonly ParameterInfo[];

  constructor(info: readonly ParameterInfo[], parts: readonly string[]) {
    this.info = info;
    this.values = new Float32Array(info.length);
    this.partValues = new Float32Array(parts.length).fill(1);
    info.forEach((parameter, at) => this.index.set(parameter.id, at));
    parts.forEach((part, at) => this.partIndex.set(part, at));
    this.reset();
  }

  reset(): void {
    for (let at = 0; at < this.info.length; at += 1) this.values[at] = this.info[at]!.def;
    this.partValues.fill(1);
  }

  has(id: string): boolean {
    return this.index.has(id);
  }

  get(id: string): number | undefined {
    const at = this.index.get(id);
    return at === undefined ? undefined : this.values[at];
  }

  /** Clamped to the parameter's range; unknown ids are ignored (models differ). */
  set(id: string, value: number): void {
    const at = this.index.get(id);
    if (at === undefined) return;
    const parameter = this.info[at]!;
    this.values[at] = Math.min(parameter.max, Math.max(parameter.min, value));
  }

  blendToward(id: string, target: number, weight: number): void {
    const current = this.get(id);
    if (current !== undefined) this.set(id, current + (target - current) * weight);
  }

  blendPart(id: string, target: number, weight: number): void {
    const at = this.partIndex.get(id);
    if (at !== undefined) this.partValues[at] = this.partValues[at]! + (target - this.partValues[at]!) * weight;
  }
}
