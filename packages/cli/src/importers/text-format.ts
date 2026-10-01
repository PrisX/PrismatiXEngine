/**
 * Number and JSON spellings shared by importers. They follow printf-style
 * rules (`%g`, round-half-even) so that re-imports keep producing the same
 * Story text for the same source values.
 */

/** `%.{precision}g`: shortest of fixed or exponent notation, no trailing zeros. */
export function formatG(value: number, precision = 6): string {
  if (!Number.isFinite(value)) return value > 0 ? "inf" : value < 0 ? "-inf" : "nan";
  if (value === 0) return Object.is(value, -0) ? "-0" : "0";
  const exponential = value.toExponential(Math.max(0, precision - 1));
  const [mantissa, exponentText] = exponential.split("e") as [string, string];
  const exponent = Number(exponentText);
  const strip = (text: string) => text.includes(".") ? text.replace(/0+$/u, "").replace(/\.$/u, "") : text;
  if (exponent < -4 || exponent >= precision) {
    const sign = exponent < 0 ? "-" : "+";
    return `${strip(mantissa)}e${sign}${String(Math.abs(exponent)).padStart(2, "0")}`;
  }
  return strip(value.toFixed(Math.max(0, precision - 1 - exponent)));
}

/** Round half to even, as Python's round() does for floats. */
export function roundHalfEven(value: number): number {
  const floor = Math.floor(value);
  const difference = value - floor;
  if (difference > 0.5) return floor + 1;
  if (difference < 0.5) return floor;
  return floor % 2 === 0 ? floor : floor + 1;
}

/** Shortest round-trip spelling of a JSON number. */
export function formatNumber(value: number): string {
  if (Number.isInteger(value) && Math.abs(value) < 1e16) return String(value);
  const text = String(value);
  if (!/e/u.test(text)) {
    // Use exponent notation where printf-style repr does (|x| < 1e-4 or >= 1e16).
    const magnitude = Math.abs(value);
    if (magnitude !== 0 && (magnitude < 1e-4 || magnitude >= 1e16)) return exponentRepr(value);
    return text;
  }
  return exponentRepr(value);
}

function exponentRepr(value: number): string {
  const [mantissa, exponentText] = value.toExponential().split("e") as [string, string];
  const exponent = Number(exponentText);
  return `${mantissa}e${exponent < 0 ? "-" : "+"}${String(Math.abs(exponent)).padStart(2, "0")}`;
}

/** Compact JSON (`,` and `:` separators, raw non-ASCII) for any value,
 * including Maps with numeric keys (written as string keys). */
export function compactJson(value: unknown): string {
  if (value === null || value === undefined) return "null";
  if (typeof value === "string") return JSON.stringify(value);
  if (typeof value === "number") return formatNumber(value);
  if (typeof value === "boolean") return value ? "true" : "false";
  if (Array.isArray(value)) return `[${value.map(compactJson).join(",")}]`;
  if (value instanceof Map)
    return `{${[...value].map(([key, item]) =>
      `${JSON.stringify(typeof key === "number" ? formatNumber(key) : String(key))}:${compactJson(item)}`).join(",")}}`;
  if (typeof value === "object")
    return `{${Object.entries(value as Record<string, unknown>).map(([key, item]) =>
      `${JSON.stringify(key)}:${compactJson(item)}`).join(",")}}`;
  throw new TypeError(`Cannot encode ${typeof value} as JSON`);
}

/** A whole number as an integer, otherwise `%g`. */
export function formatCoordinate(value: number): string {
  return Number.isInteger(value) ? String(value) : formatG(value);
}
