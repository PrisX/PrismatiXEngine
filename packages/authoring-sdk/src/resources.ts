const uuidPattern = /^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$/u;
const assetPrefix = "asset:";

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
