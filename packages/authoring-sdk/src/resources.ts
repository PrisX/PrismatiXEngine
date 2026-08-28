const uuidPattern = /^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$/u;
const assetPrefix = "asset:";

export interface ProjectAssetDescriptor {
  readonly id: string;
  readonly source: string;
  readonly kind: string;
  readonly name?: string;
}

export type ResolvedResourceReference =
  | {readonly kind: "path"; readonly path: string}
  | {
      readonly kind: "asset";
      readonly assetId: string;
      readonly path?: string;
      readonly asset?: ProjectAssetDescriptor;
    };

/**
 * Canonical Runtime IR token for a resource that needs stable identity across
 * file moves or renames. Ordinary authored resources should remain project-
 * relative paths unless stable identity is actually needed.
 */
export function assetToken(id: string): string {
  if (!uuidPattern.test(id)) throw new TypeError("PrismatiX asset identity must be a UUID");
  return `${assetPrefix}${id.toLowerCase()}`;
}

export function isAssetToken(value: string): boolean {
  return value.startsWith(assetPrefix) && uuidPattern.test(value.slice(assetPrefix.length));
}

export function assetIdFromToken(value: string): string | undefined {
  return isAssetToken(value) ? value.slice(assetPrefix.length).toLowerCase() : undefined;
}

export type ResourceReferenceKind = "path" | "asset";

/**
 * Classifies the two resource forms accepted by the authoring/runtime bridge.
 * This helper intentionally does not turn every path into an asset catalog
 * entry: the project catalog is an opt-in stable-identity layer, not a virtual
 * filesystem or mandatory asset database.
 */
export function resourceReferenceKind(value: string): ResourceReferenceKind {
  return isAssetToken(value) ? "asset" : "path";
}

/**
 * Resolves the same `asset:<uuid>` form consumed by RuntimeAssetResolver while
 * leaving ordinary project-relative paths untouched. Missing catalog entries
 * stay identifiable by UUID so an Editor can surface or repair the broken
 * reference without silently falling back to a stale path.
 */
export function resolveResourceReference(
  value: string,
  assets: readonly ProjectAssetDescriptor[] = [],
): ResolvedResourceReference {
  const assetId = assetIdFromToken(value);
  if (assetId === undefined) return {kind: "path", path: value};

  const asset = assets.find((candidate) => candidate.id.toLowerCase() === assetId);
  return asset === undefined
    ? {kind: "asset", assetId}
    : {kind: "asset", assetId, path: asset.source, asset};
}

/**
 * Explicitly opts a known catalog asset into stable-identity authoring. This
 * is intentionally not automatic: ordinary files stay path-first, while UI,
 * character expressions, galleries, or other rename-stable references can
 * request a catalog token when they need one.
 */
export function stableResourceReference(asset: ProjectAssetDescriptor): string {
  return assetToken(asset.id);
}
