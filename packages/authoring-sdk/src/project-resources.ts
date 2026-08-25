import {validateDocument} from "./contracts.js";
import type {AuthoringDiagnostic, Result} from "./types.js";

type ObjectValue = Record<string, unknown>;

const characterAssetKinds = new Set(["character", "cg", "uiImage"]);

function object(value: unknown): ObjectValue | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as ObjectValue
    : undefined;
}

function objects(value: unknown): ObjectValue[] {
  return Array.isArray(value)
    ? value.map(object).filter((item): item is ObjectValue => item !== undefined)
    : [];
}

function diagnostic(code: string, message: string, path?: string, details?: string): AuthoringDiagnostic {
  return {
    severity: "error",
    code,
    message,
    ...(path === undefined ? {} : {path}),
    ...(details === undefined ? {} : {details}),
  };
}

export interface ProjectCharacterResourceValidationOptions {
  /** Logical path used for project-level diagnostics. */
  readonly projectPath?: string;
  /** Optional VFS/filesystem existence check mirroring CharacterResources. */
  readonly assetExists?: (path: string) => boolean;
}

/**
 * Performs the cross-document checks that cannot be expressed by validating a
 * single .pxproject or .pxcharacter document in isolation.
 *
 * `characterDocuments` is keyed by the exact project-relative source stored in
 * the project character descriptor. Keeping IO outside the SDK makes this API
 * usable by Node tools, browser Editors, tests, and virtual project providers.
 */
export function validateProjectCharacterResources(
  project: unknown,
  characterDocuments: Readonly<Record<string, unknown>>,
  options: ProjectCharacterResourceValidationOptions = {},
): Result<true> {
  const projectPath = options.projectPath ?? "project.pxproject";
  const projectValidation = validateDocument<ObjectValue>("project", project, projectPath);
  if (!projectValidation.valid || projectValidation.value === undefined) {
    return {diagnostics: projectValidation.diagnostics, valid: false};
  }

  const root = projectValidation.value;
  if (root.characters === undefined) return {value: true, diagnostics: [], valid: true};

  const diagnostics: AuthoringDiagnostic[] = [];
  const descriptors = objects(root.characters);
  if (!Array.isArray(root.assets)) {
    diagnostics.push(diagnostic(
      "PXSDKRES2001",
      "A project declaring character resources must also declare the asset catalog required by the Runtime loader",
      projectPath,
    ));
    return {diagnostics, valid: false};
  }

  const assets = objects(root.assets);
  const assetsById = new Map<string, ObjectValue>();
  for (const asset of assets) {
    if (typeof asset.id === "string") assetsById.set(asset.id, asset);
  }

  const expressionOwners = new Map<string, string>();
  const characterLookupOwners = new Map<string, string>();

  const ownLookup = (key: unknown, owner: string): void => {
    if (typeof key !== "string" || key.length === 0) return;
    const previous = characterLookupOwners.get(key);
    if (previous !== undefined && previous !== owner) {
      diagnostics.push(diagnostic(
        "PXSDKRES2009",
        "Character runtime lookup key is ambiguous across project character documents",
        projectPath,
        key,
      ));
      return;
    }
    characterLookupOwners.set(key, owner);
  };

  const checkAssetReference = (
    assetId: unknown,
    sourcePath: string,
    characterId: string,
    expressionId: string,
    property: string,
  ): void => {
    if (typeof assetId !== "string") return;
    const asset = assetsById.get(assetId);
    if (asset === undefined) {
      diagnostics.push(diagnostic(
        "PXSDKRES2006",
        `Character expression ${property} references an asset UUID missing from project.pxproject`,
        sourcePath,
        `${characterId}:${expressionId}:${assetId}`,
      ));
      return;
    }
    if (typeof asset.kind !== "string" || !characterAssetKinds.has(asset.kind)) {
      diagnostics.push(diagnostic(
        "PXSDKRES2007",
        `Character expression ${property} requires an asset of kind character, cg, or uiImage`,
        sourcePath,
        `${expressionId}:${assetId}:${String(asset.kind ?? "")}`,
      ));
      return;
    }
    if (typeof asset.source === "string" && options.assetExists !== undefined && !options.assetExists(asset.source)) {
      diagnostics.push(diagnostic(
        "PXSDKRES2008",
        `Character expression ${property} asset file is missing`,
        asset.source,
        `${expressionId}:${assetId}`,
      ));
    }
  };

  for (const descriptor of descriptors) {
    const source = typeof descriptor.source === "string" ? descriptor.source : undefined;
    const descriptorId = typeof descriptor.id === "string" ? descriptor.id : "";
    const descriptorName = typeof descriptor.displayName === "string" ? descriptor.displayName : "";
    if (source === undefined) continue;

    const authored = characterDocuments[source];
    if (authored === undefined) {
      diagnostics.push(diagnostic(
        "PXSDKRES2002",
        "Project character descriptor source is unavailable",
        source,
        descriptorId,
      ));
      continue;
    }

    const characterValidation = validateDocument<ObjectValue>("character", authored, source);
    diagnostics.push(...characterValidation.diagnostics);
    if (!characterValidation.valid || characterValidation.value === undefined) continue;
    const character = characterValidation.value;
    const characterId = typeof character.id === "string" ? character.id : descriptorId;

    if (character.id !== descriptor.id) {
      diagnostics.push(diagnostic(
        "PXSDKRES2003",
        "Character document id does not match its project descriptor",
        source,
        `${String(descriptor.id)} != ${String(character.id)}`,
      ));
    }
    if (character.displayName !== descriptor.displayName) {
      diagnostics.push(diagnostic(
        "PXSDKRES2004",
        "Character document displayName does not match its project descriptor",
        source,
        `${descriptorName} != ${String(character.displayName)}`,
      ));
    }

    const aliases = Array.isArray(character.aliases) ? character.aliases : [];
    if (aliases.length > 128) {
      diagnostics.push(diagnostic(
        "PXSDKRES2010",
        "Character aliases exceed the Runtime limit of 128 entries",
        source,
        characterId,
      ));
    }

    ownLookup(character.id, characterId);
    ownLookup(character.displayName, characterId);
    for (const alias of aliases) ownLookup(alias, characterId);

    for (const expression of objects(character.expressions)) {
      const expressionId = typeof expression.id === "string" ? expression.id : "";
      const previousOwner = expressionOwners.get(expressionId);
      if (expressionId.length > 0 && previousOwner !== undefined && previousOwner !== characterId) {
        diagnostics.push(diagnostic(
          "PXSDKRES2005",
          "Character expression UUIDs must be globally unique across the project",
          source,
          expressionId,
        ));
      } else if (expressionId.length > 0) {
        expressionOwners.set(expressionId, characterId);
      }

      const expressionAliases = Array.isArray(expression.aliases) ? expression.aliases : [];
      if (expressionAliases.length > 128) {
        diagnostics.push(diagnostic(
          "PXSDKRES2011",
          "Character expression aliases exceed the Runtime limit of 128 entries",
          source,
          expressionId,
        ));
      }

      checkAssetReference(expression.assetId, source, characterId, expressionId, "assetId");
      if (expression.thumbnailAssetId !== undefined && expression.thumbnailAssetId !== null) {
        checkAssetReference(expression.thumbnailAssetId, source, characterId, expressionId, "thumbnailAssetId");
      }
      // portraitAssetId is currently authoring metadata. It is intentionally
      // not treated as a Runtime-required CharacterResources reference until
      // the native Runtime exposes portrait semantics.
    }
  }

  return diagnostics.length === 0
    ? {value: true, diagnostics: [], valid: true}
    : {diagnostics, valid: false};
}
