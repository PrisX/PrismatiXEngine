// SemVer versions and ranges for extension packages: what `prismatix
// extension add owner/name@<range>` asks for and how a registry resolves it.
// Pure JavaScript (no Node.js APIs) so a registry can share it.

export interface SemVer {
  readonly major: number;
  readonly minor: number;
  readonly patch: number;
  /** Dot-separated prerelease identifiers (`1.0.0-beta.2` -> ["beta", 2]). */
  readonly prerelease: readonly (string | number)[];
}

const versionPattern = /^[vV]?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z.-]+)?$/u;

/** A full `major.minor.patch[-prerelease][+build]` version, or undefined. */
export function parseSemVer(value: string): SemVer | undefined {
  const match = versionPattern.exec(value.trim());
  if (match === null) return undefined;
  const numbers = [match[1], match[2], match[3]].map(Number);
  if (numbers.some((number) => !Number.isSafeInteger(number))) return undefined;
  return {major: numbers[0]!, minor: numbers[1]!, patch: numbers[2]!,
    prerelease: match[4] === undefined ? [] : match[4].split(".").map((part) => /^\d+$/u.test(part) ? Number(part) : part)};
}

/** SemVer precedence: negative, zero or positive. Build metadata is ignored. */
export function compareSemVer(left: SemVer | string, right: SemVer | string): number {
  const a = typeof left === "string" ? parseSemVer(left) : left;
  const b = typeof right === "string" ? parseSemVer(right) : right;
  if (a === undefined || b === undefined) throw new RangeError(`Not a SemVer version: ${String(a === undefined ? left : right)}`);
  for (const key of ["major", "minor", "patch"] as const) if (a[key] !== b[key]) return a[key] < b[key] ? -1 : 1;
  if (a.prerelease.length === 0 || b.prerelease.length === 0) return b.prerelease.length - a.prerelease.length;
  for (let index = 0; index < Math.max(a.prerelease.length, b.prerelease.length); index += 1) {
    const x = a.prerelease[index];
    const y = b.prerelease[index];
    if (x === undefined) return -1;
    if (y === undefined) return 1;
    if (x === y) continue;
    if (typeof x === "number" && typeof y === "number") return x < y ? -1 : 1;
    if (typeof x === "number") return -1;
    if (typeof y === "number") return 1;
    return x < y ? -1 : 1;
  }
  return 0;
}

type Comparator = {readonly operator: "<" | "<=" | ">" | ">=" | "="; readonly version: SemVer};

function bound(major: number, minor: number, patch: number): SemVer {
  return {major, minor, patch, prerelease: []};
}

// One token of a range: `>=1.2.0`, `^1.2.0`, `~1.2`, `1.x`, `1`, `*`.
function comparators(token: string): Comparator[] | undefined {
  if (token === "*" || token === "x" || token === "X") return [];
  const match = /^(>=|<=|>|<|=|\^|~)?[vV]?(\d+|[xX*])(?:\.(\d+|[xX*]))?(?:\.(\d+|[xX*]))?(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$/u.exec(token);
  if (match === null) return undefined;
  const operator = match[1] ?? "";
  const wild = (part: string | undefined) => part === undefined || /^[xX*]$/u.test(part);
  const parts = [match[2], match[3], match[4]];
  const firstWild = parts.findIndex(wild);
  const numbers = parts.map((part) => wild(part) ? 0 : Number(part));
  const [major, minor, patch] = numbers as [number, number, number];
  if (firstWild === 0) return operator === "<" || operator === ">" ? undefined : [];
  if (firstWild > 0) {
    // A partial version (`1`, `1.2`, `1.x`) is the range it names.
    if (match[5] !== undefined) return undefined;
    const upper = firstWild === 1 ? bound(major + 1, 0, 0) : bound(major, minor + 1, 0);
    const lower = bound(major, firstWild === 1 ? 0 : minor, 0);
    switch (operator) {
      case "": case "=": case "^": case "~":
        return operator === "^" && firstWild === 2 && major === 0
          ? [{operator: ">=", version: lower}, {operator: "<", version: bound(0, minor + 1, 0)}]
          : operator === "^" && firstWild === 2
            ? [{operator: ">=", version: lower}, {operator: "<", version: bound(major + 1, 0, 0)}]
            : [{operator: ">=", version: lower}, {operator: "<", version: upper}];
      case ">=": return [{operator: ">=", version: lower}];
      case "<": return [{operator: "<", version: lower}];
      case ">": return [{operator: ">=", version: upper}];
      case "<=": return [{operator: "<", version: upper}];
    }
    return undefined;
  }
  const version = parseSemVer(`${major}.${minor}.${patch}${match[5] ?? ""}`);
  if (version === undefined) return undefined;
  if (operator === "^") {
    const upper = major > 0 ? bound(major + 1, 0, 0) : minor > 0 ? bound(0, minor + 1, 0) : bound(0, 0, patch + 1);
    return [{operator: ">=", version}, {operator: "<", version: upper}];
  }
  if (operator === "~") return [{operator: ">=", version}, {operator: "<", version: bound(major, minor + 1, 0)}];
  return [{operator: (operator === "" ? "=" : operator) as Comparator["operator"], version}];
}

/** The comparator sets of a range (`||`-joined, space-separated), or undefined when it is not a range. */
function parseRange(range: string): Comparator[][] | undefined {
  const text = range.trim();
  if (text.length === 0 || text.length > 256) return undefined;
  const sets: Comparator[][] = [];
  for (const clause of text.split("||").map((part) => part.trim())) {
    if (clause.length === 0) return undefined;
    const set: Comparator[] = [];
    for (const token of clause.replace(/(>=|<=|>|<|=|\^|~)\s+/gu, "$1").split(/\s+/u)) {
      const parsed = comparators(token);
      if (parsed === undefined) return undefined;
      set.push(...parsed);
    }
    sets.push(set);
  }
  return sets;
}

/** True when `range` is a range this module understands. */
export function isVersionRange(range: string): boolean {
  return parseRange(range) !== undefined;
}

/** True when `range` names one exact version (`1.2.3`, `=1.2.3`, `v1.2.3`). */
export function isExactVersion(range: string): boolean {
  return parseSemVer(range.trim().replace(/^=\s*/u, "")) !== undefined;
}

/**
 * Whether `version` satisfies `range`: comparators `>=`, `<=`, `>`, `<`, `=`,
 * `^`, `~`, partial versions (`1`, `1.2`, `1.x`) and `*`, space-separated
 * (all must hold) and joined by `||` (any set). As in npm, a prerelease only
 * satisfies a set with a comparator on its own major.minor.patch that has a
 * prerelease too, unless `includePrerelease` is set. Undefined when the range
 * or version is not SemVer.
 */
export function satisfiesVersionRange(version: string, range: string,
  options: {readonly includePrerelease?: boolean} = {}): boolean | undefined {
  const parsed = parseSemVer(version);
  const sets = parseRange(range);
  if (parsed === undefined || sets === undefined) return undefined;
  return sets.some((set) => {
    if (!set.every(({operator, version: required}) => {
      const compared = compareSemVer(parsed, required);
      return operator === "=" ? compared === 0 : operator === ">" ? compared > 0 : operator === ">=" ? compared >= 0
        : operator === "<" ? compared < 0 : compared <= 0;
    })) return false;
    if (parsed.prerelease.length === 0 || options.includePrerelease === true) return true;
    return set.some(({version: required}) => required.prerelease.length > 0 && required.major === parsed.major &&
      required.minor === parsed.minor && required.patch === parsed.patch);
  });
}

/** The highest of `versions` that satisfies `range`, or undefined. */
export function maxSatisfyingVersion(versions: Iterable<string>, range: string): string | undefined {
  let best: string | undefined;
  for (const version of versions)
    if (satisfiesVersionRange(version, range) === true && (best === undefined || compareSemVer(version, best) > 0)) best = version;
  return best;
}
