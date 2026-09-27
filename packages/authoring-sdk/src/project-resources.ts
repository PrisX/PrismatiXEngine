import {parseDocument, validateDocument} from "./contracts.js";
import {createJsonSourceLocator} from "./json-source-spans.js";
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
  /** Exact author text for project and character source diagnostics. */
  readonly projectSource?: {readonly path: string; readonly text: string};
  readonly characterSources?: Readonly<Record<string, string>>;
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
  const locate = createJsonSourceLocator((path) => {
    if (path === projectPath) return options.projectSource;
    const authored = characterDocuments[path];
    const text = options.characterSources?.[path] ??
      (typeof authored === "string" ? authored : undefined);
    return text === undefined ? undefined : {path, text};
  });
  const at = (value: AuthoringDiagnostic, sourcePath: string,
    pointer: string, expected: unknown): AuthoringDiagnostic => {
    const span = locate(sourcePath, pointer, expected);
    return span === undefined ? value : {...value, span};
  };
  const descriptors = objects(root.characters);
  if (!Array.isArray(root.assets)) {
    diagnostics.push(at(diagnostic(
      "PXSDKRES2001",
      "A project declaring character resources must also declare the asset catalog required by the Runtime loader",
      projectPath,
    ), projectPath, "/characters", root.characters));
    return {diagnostics, valid: false};
  }

  const assets = objects(root.assets);
  const assetsById = new Map<string, {asset: ObjectValue; index: number}>();
  for (const [index, asset] of assets.entries()) {
    if (typeof asset.id === "string") assetsById.set(asset.id.toLowerCase(), {asset, index});
  }

  const expressionOwners = new Map<string, string>();
  const characterLookupOwners = new Map<string, string>();

  const ownLookup = (key: unknown, owner: string, sourcePath: string,
    pointer: string): void => {
    if (typeof key !== "string" || key.length === 0) return;
    const previous = characterLookupOwners.get(key);
    if (previous !== undefined && previous !== owner) {
      diagnostics.push(at(diagnostic(
        "PXSDKRES2009",
        "Character runtime lookup key is ambiguous across project character documents",
        projectPath,
        key,
      ), sourcePath, pointer, key));
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
    pointer: string,
  ): void => {
    if (typeof assetId !== "string") return;
    const resolved = assetsById.get(assetId.toLowerCase());
    if (resolved === undefined) {
      diagnostics.push(at(diagnostic(
        "PXSDKRES2006",
        `Character expression ${property} references an asset UUID missing from project.pxproject`,
        sourcePath,
        `${characterId}:${expressionId}:${assetId}`,
      ), sourcePath, pointer, assetId));
      return;
    }
    const {asset, index} = resolved;
    if (typeof asset.kind !== "string" || !characterAssetKinds.has(asset.kind)) {
      diagnostics.push(at(diagnostic(
        "PXSDKRES2007",
        `Character expression ${property} requires an asset of kind character, cg, or uiImage`,
        sourcePath,
        `${expressionId}:${assetId}:${String(asset.kind ?? "")}`,
      ), projectPath, `/assets/${index}/kind`, asset.kind));
      return;
    }
    if (typeof asset.source === "string" && options.assetExists !== undefined && !options.assetExists(asset.source)) {
      diagnostics.push(at(diagnostic(
        "PXSDKRES2008",
        `Character expression ${property} asset file is missing`,
        asset.source,
        `${expressionId}:${assetId}`,
      ), projectPath, `/assets/${index}/source`, asset.source));
    }
  };

  for (const [descriptorIndex, descriptor] of descriptors.entries()) {
    const source = typeof descriptor.source === "string" ? descriptor.source : undefined;
    const descriptorId = typeof descriptor.id === "string" ? descriptor.id : "";
    const descriptorName = typeof descriptor.displayName === "string" ? descriptor.displayName : "";
    if (source === undefined) continue;

    const authored = characterDocuments[source];
    if (authored === undefined) {
      diagnostics.push(at(diagnostic(
        "PXSDKRES2002",
        "Project character descriptor source is unavailable",
        source,
        descriptorId,
      ), projectPath, `/characters/${descriptorIndex}/source`, source));
      continue;
    }

    const characterValidation = typeof authored === "string"
      ? parseDocument<ObjectValue>("character", authored, source)
      : validateDocument<ObjectValue>("character", authored, source);
    diagnostics.push(...characterValidation.diagnostics);
    if (!characterValidation.valid || characterValidation.value === undefined) continue;
    const character = characterValidation.value;
    const characterId = typeof character.id === "string" ? character.id : descriptorId;

    if (typeof character.id !== "string" || typeof descriptor.id !== "string" ||
        character.id.toLowerCase() !== descriptor.id.toLowerCase()) {
      const location = typeof character.id === "string"
        ? [source, "/id", character.id] as const
        : [projectPath, `/characters/${descriptorIndex}/id`, descriptor.id] as const;
      diagnostics.push(at(diagnostic(
        "PXSDKRES2003",
        "Character document id does not match its project descriptor",
        source,
        `${String(descriptor.id)} != ${String(character.id)}`,
      ), location[0], location[1], location[2]));
    }
    if (character.displayName !== descriptor.displayName) {
      const location = typeof character.displayName === "string"
        ? [source, "/displayName", character.displayName] as const
        : [projectPath, `/characters/${descriptorIndex}/displayName`, descriptor.displayName] as const;
      diagnostics.push(at(diagnostic(
        "PXSDKRES2004",
        "Character document displayName does not match its project descriptor",
        source,
        `${descriptorName} != ${String(character.displayName)}`,
      ), location[0], location[1], location[2]));
    }

    const aliases = Array.isArray(character.aliases) ? character.aliases : [];
    if (aliases.length > 128) {
      diagnostics.push(at(diagnostic(
        "PXSDKRES2010",
        "Character aliases exceed the Runtime limit of 128 entries",
        source,
        characterId,
      ), source, "/aliases", aliases));
    }

    ownLookup(character.id, characterId, source, "/id");
    ownLookup(character.displayName, characterId, source, "/displayName");
    for (const [aliasIndex, alias] of aliases.entries())
      ownLookup(alias, characterId, source, `/aliases/${aliasIndex}`);

    for (const [expressionIndex, expression] of objects(character.expressions).entries()) {
      const expressionId = typeof expression.id === "string" ? expression.id : "";
      const previousOwner = expressionOwners.get(expressionId);
      if (expressionId.length > 0 && previousOwner !== undefined && previousOwner !== characterId) {
        diagnostics.push(at(diagnostic(
          "PXSDKRES2005",
          "Character expression UUIDs must be globally unique across the project",
          source,
          expressionId,
        ), source, `/expressions/${expressionIndex}/id`, expressionId));
      } else if (expressionId.length > 0) {
        expressionOwners.set(expressionId, characterId);
      }

      const expressionAliases = Array.isArray(expression.aliases) ? expression.aliases : [];
      if (expressionAliases.length > 128) {
        diagnostics.push(at(diagnostic(
          "PXSDKRES2011",
          "Character expression aliases exceed the Runtime limit of 128 entries",
          source,
          expressionId,
        ), source, `/expressions/${expressionIndex}/aliases`, expressionAliases));
      }

      checkAssetReference(expression.assetId, source, characterId, expressionId, "assetId",
        `/expressions/${expressionIndex}/assetId`);
      if (expression.thumbnailAssetId !== undefined && expression.thumbnailAssetId !== null) {
        checkAssetReference(expression.thumbnailAssetId, source, characterId, expressionId,
          "thumbnailAssetId", `/expressions/${expressionIndex}/thumbnailAssetId`);
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
