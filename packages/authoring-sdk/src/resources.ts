const uuidPattern = /^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$/u;
const assetPrefix = "asset:";
const drivePathPattern = /^[A-Za-z]:/u;

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

/** Returns the stable asset identity carried by either the canonical token or
 * the legacy bare-UUID spelling. Bare UUIDs remain accepted at the authoring
 * boundary, but new Runtime IR always uses `asset:<uuid>`. */
export function assetIdFromReference(value: string): string | undefined {
  const tokenId = assetIdFromToken(value);
  if (tokenId !== undefined) return tokenId;
  return uuidPattern.test(value) ? value.toLowerCase() : undefined;
}

/** Mirrors the safe project-relative path contract without requiring callers
 * to run a complete JSON document through AJV. */
export function isSafeResourcePath(value: string): boolean {
  if (value.length === 0 || value.length > 4096 || value.startsWith(assetPrefix) ||
      value.startsWith("/") || drivePathPattern.test(value) || value.includes("\\")) {
    return false;
  }
  return !value.split("/").includes("..");
}

export type ResourceReferenceKind = "path" | "asset";

/**
 * Classifies the two resource forms accepted by the authoring/runtime bridge.
 * This helper intentionally does not turn every path into an asset catalog
 * entry: the project catalog is an opt-in stable-identity layer, not a virtual
 * filesystem or mandatory asset database.
 */
export function resourceReferenceKind(value: string): ResourceReferenceKind {
  return assetIdFromReference(value) === undefined ? "path" : "asset";
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
  const assetId = assetIdFromReference(value);
  if (assetId === undefined) return {kind: "path", path: value};

  const asset = assets.find((candidate) => candidate.id.toLowerCase() === assetId);
  return asset === undefined
    ? {kind: "asset", assetId}
    : {kind: "asset", assetId, path: asset.source, asset};
}

/** Resolves an authored resource to the exact Runtime IR spelling and its
 * package source. The source is intentionally explicit so build tools do not
 * have to rediscover dependencies by parsing Runtime IR strings. */
export interface CanonicalResourceReference {
  readonly reference: string;
  readonly source: string;
  readonly assetId?: string;
  readonly asset?: ProjectAssetDescriptor;
}

export function canonicalResourceReference(
  value: string,
  assets: readonly ProjectAssetDescriptor[] = [],
): CanonicalResourceReference | undefined {
  const resolved = resolveResourceReference(value, assets);
  if (resolved.kind === "path") {
    return isSafeResourcePath(resolved.path)
      ? {reference: resolved.path, source: resolved.path}
      : undefined;
  }
  if (resolved.path === undefined) return undefined;
  return {
    reference: assetToken(resolved.assetId),
    source: resolved.path,
    assetId: resolved.assetId,
    ...(resolved.asset === undefined ? {} : {asset: resolved.asset}),
  };
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

export type ProjectDependencyKind = "resource" | "extension";

export interface ProjectDependency {
  /** Virtual path written into the build/package. */
  readonly logicalPath: string;
  /** Project-relative input path read by the build tool. */
  readonly source: string;
  readonly kind: ProjectDependencyKind;
  readonly assetId?: string;
  readonly origin?: string;
}

export interface ProjectDependencyReference {
  readonly value: string;
  readonly origin?: string;
}

export interface ProjectExtensionDependency {
  readonly manifestPath: string;
  readonly entry: string;
  readonly modules?: readonly string[];
}

export interface CollectProjectDependenciesInput {
  readonly assets?: readonly ProjectAssetDescriptor[];
  readonly references?: readonly ProjectDependencyReference[];
  readonly extensions?: readonly ProjectExtensionDependency[];
}

export interface ProjectDependencyCollection {
  readonly dependencies: readonly ProjectDependency[];
  readonly diagnostics: readonly {
    readonly severity: "error";
    readonly code: string;
    readonly message: string;
    readonly path?: string;
    readonly details?: string;
  }[];
  readonly valid: boolean;
}

/**
 * Produces the complete set of external package inputs discovered by the SDK.
 * Catalog entries are included even when they are not statically referenced,
 * preserving dynamic Runtime lookup. Static raw paths are included even when
 * they do not have a catalog row.
 */
export function collectProjectDependencies(
  input: CollectProjectDependenciesInput,
): ProjectDependencyCollection {
  const diagnostics: ProjectDependencyCollection["diagnostics"][number][] = [];
  const dependencies = new Map<string, ProjectDependency>();
  const add = (dependency: ProjectDependency): void => {
    if (!isSafeResourcePath(dependency.logicalPath) || !isSafeResourcePath(dependency.source)) {
      diagnostics.push({
        severity: "error",
        code: "PXBUILD1041",
        message: "Project dependency must use a safe project-relative path",
        ...(dependency.origin === undefined ? {} : {path: dependency.origin}),
        details: dependency.source,
      });
      return;
    }
    const key = dependency.logicalPath.toLocaleLowerCase("en-US");
    const previous = dependencies.get(key);
    if (previous !== undefined && previous.source !== dependency.source) {
      diagnostics.push({
        severity: "error",
        code: "PXBUILD1042",
        message: "Project dependencies collide at the same logical path",
        ...(dependency.origin === undefined ? {} : {path: dependency.origin}),
        details: `${previous.source} != ${dependency.source}`,
      });
      return;
    }
    if (previous === undefined) dependencies.set(key, dependency);
  };

  for (const asset of input.assets ?? []) {
    add({
      logicalPath: asset.source,
      source: asset.source,
      kind: "resource",
      assetId: asset.id.toLowerCase(),
      origin: "project.assets",
    });
  }
  for (const reference of input.references ?? []) {
    const resolved = canonicalResourceReference(reference.value, input.assets ?? []);
    if (resolved === undefined) {
      diagnostics.push({
        severity: "error",
        code: "PXBUILD1043",
        message: "Resource dependency is neither a safe path nor a cataloged asset identity",
        ...(reference.origin === undefined ? {} : {path: reference.origin}),
        details: reference.value,
      });
      continue;
    }
    add({
      logicalPath: resolved.source,
      source: resolved.source,
      kind: "resource",
      ...(resolved.assetId === undefined ? {} : {assetId: resolved.assetId}),
      ...(reference.origin === undefined ? {} : {origin: reference.origin}),
    });
  }
  for (const extension of input.extensions ?? []) {
    const slash = extension.manifestPath.lastIndexOf("/");
    const prefix = slash < 0 ? "" : extension.manifestPath.slice(0, slash + 1);
    for (const relativePath of [extension.entry, ...(extension.modules ?? [])]) {
      const source = `${prefix}${relativePath}`;
      add({
        logicalPath: source,
        source,
        kind: "extension",
        origin: extension.manifestPath,
      });
    }
  }

  return {
    dependencies: [...dependencies.values()].sort((left, right) =>
      left.logicalPath.localeCompare(right.logicalPath, "en-US")),
    diagnostics,
    valid: diagnostics.length === 0,
  };
}
