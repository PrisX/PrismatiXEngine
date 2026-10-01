import {storyAssetRootsError, type StoryAssetRoots} from "./story-assets.js";
import {storyMacroFilesError} from "./story-macros.js";
import {validateDocument} from "./contracts.js";
import {fnv1a64 as hash64} from "./fnv1a64.js";
import {isSafeResourcePath} from "./resources.js";
import type {ProjectDocument} from "./project.js";
import type {AuthoringDiagnostic, CharacterDocument, CharacterExpression,
  Result} from "./types.js";

type ObjectValue = Record<string, unknown>;

/** Project-source character expression. `assetPath` is lowered to the native
 * asset catalog; explicit ids remain available for save-compatible renames. */
export type CharacterSourceExpression = Omit<CharacterExpression, "id" | "assetId"> & {
  readonly id?: string;
  readonly thumbnailAssetId?: string | null;
  readonly portraitAssetId?: string;
  /** Message-window face image; lowered to `faceAssetId`. */
  readonly facePath?: string;
} & ({readonly assetPath: string; readonly assetId?: string} |
  {readonly assetId: string; readonly assetPath?: never});

/** Author-only character document shape consumed by `normalizeProject` and
 * `compileProject`. The published .pxcharacter retains the strict canonical
 * `CharacterDocument` format. */
export interface CharacterSourceDocument extends
  Omit<CharacterDocument, "id" | "displayName" | "aliases" | "expressions"> {
  readonly id?: string;
  readonly revision?: number;
  readonly displayName?: string;
  readonly aliases?: readonly string[];
  readonly expressions: readonly CharacterSourceExpression[];
}

export interface NormalizeProjectInput {
  /** A complete project, a small source manifest, JSON text, or nothing. */
  readonly project?: unknown;
  readonly documents?: Readonly<Record<string, unknown>>;
  readonly sourceFiles?: Readonly<Record<string, string>>;
  readonly locales?: Readonly<Record<string, unknown>>;
  readonly projectPath?: string;
  /** Conventional single-file Story entry. Defaults to Story/main.pxstory. */
  readonly storyPath?: string;
  /** Used only when the source manifest does not declare defaultLocale. */
  readonly defaultLocale?: string;
}

export interface NormalizedProjectInput {
  readonly project: ProjectDocument;
  readonly documents: Readonly<Record<string, unknown>>;
  readonly sourceFiles: Readonly<Record<string, string>>;
  readonly locales: Readonly<Record<string, unknown>>;
  readonly projectPath: string;
  readonly storyPath: string;
  /** Author-only file or directory roots; never serialized into project.pxproject. */
  readonly resourceIncludes: readonly string[];
  /** Author-only Story short-name roots; never serialized into project.pxproject. */
  readonly storyAssets?: StoryAssetRoots;
  /** Author-only macro files every Story may call; never serialized into project.pxproject. */
  readonly storyMacros?: readonly string[];
  /** Packaging branding; never serialized into project.pxproject. */
  readonly distribution?: ProjectDistribution;
}

/**
 * How `prismatix pack` names the shipped game. `executableName` replaces
 * `PrismatiXPlayer` (the platform extension is kept); `icon` is a
 * project-relative `.ico` written into the Windows executable.
 */
export interface ProjectDistribution {
  readonly executableName?: string;
  readonly icon?: string;
  readonly android?: ProjectAndroidDistribution;
  readonly transcode?: ProjectTranscode;
}

/**
 * Smaller encodings for distribution builds (`prismatix build`/`pack`, not
 * `dev`). Files keep their logical paths: the Player recognises WebP and Ogg
 * Opus by content, so a ".png" may hold WebP and a ".ogg" may hold Opus.
 * `exclude` globs (`*`, `**`, `?`) match project-relative paths.
 */
export interface ProjectTranscode {
  readonly images?: {
    readonly format: "webp";
    /** 1-100 for lossy colour with exact alpha, or "lossless" (default 90). */
    readonly quality?: number | "lossless";
    /** 0 (fastest) to 6 (smallest), default 4. */
    readonly effort?: number;
  };
  readonly audio?:
    | {readonly format: "opus"; /** kbit/s, 6-256, default 96. */ readonly bitrate?: number}
    | {readonly format: "vorbis"; /** oggenc scale -1 to 10, default 4. */ readonly quality?: number};
  readonly exclude?: readonly string[];
}

/**
 * `prismatix pack --target android` settings. `icon` is a project-relative
 * square PNG. The keystore names environment variables that hold its
 * passwords; passwords themselves never belong in prismatix.json.
 */
export interface ProjectAndroidDistribution {
  readonly applicationId?: string;
  readonly versionCode?: number;
  readonly versionName?: string;
  readonly label?: string;
  readonly icon?: string;
  readonly keystore?: {
    readonly path: string;
    readonly alias: string;
    readonly storePasswordEnv: string;
    readonly keyPasswordEnv?: string;
  };
}

const reservedFileNames = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$/i;

/** Validates the author-only `distribution` block of prismatix.json. */
export function projectDistributionError(value: unknown): string | undefined {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    return "distribution must be an object";
  const {executableName, icon, output, compression, archiveShards, linkMedia, android, transcode,
    ...rest} = value as Record<string, unknown>;
  const extra = Object.keys(rest);
  if (extra.length > 0) return `distribution does not support ${extra.join(", ")}`;
  if (executableName !== undefined && (typeof executableName !== "string" ||
      executableName.length === 0 || new TextEncoder().encode(executableName).length > 64 ||
      /[\u0000-\u001f\u007f<>:"/\\|?*]/.test(executableName) ||
      /^[ .]|[ .]$/.test(executableName) || reservedFileNames.test(executableName) ||
      /\.exe$/i.test(executableName)))
    return "distribution.executableName must be a portable file name of at most 64 bytes, without .exe";
  if (icon !== undefined && (typeof icon !== "string" || !isSafeResourcePath(icon) ||
      icon.includes(":") || !/\.ico$/i.test(icon)))
    return "distribution.icon must be a safe project-relative .ico path";
  // Defaults for build/pack/release when the command line does not say.
  if (output !== undefined && (typeof output !== "string" || output.length === 0 ||
      output.length > 1024 || /[\u0000-\u001f]/.test(output)))
    return "distribution.output must be a directory path";
  if (compression !== undefined && !["none", "fast", "balanced", "maximum"].includes(compression as string))
    return "distribution.compression must be none, fast, balanced, or maximum";
  if (archiveShards !== undefined && (!Number.isInteger(archiveShards) ||
      (archiveShards as number) < 1 || (archiveShards as number) > 64))
    return "distribution.archiveShards must be an integer from 1 to 64";
  if (linkMedia !== undefined && typeof linkMedia !== "boolean")
    return "distribution.linkMedia must be true or false";
  if (transcode !== undefined) {
    const error = transcodeError(transcode);
    if (error !== undefined) return error;
  }
  return android === undefined ? undefined : androidDistributionError(android);
}

const plainObject = (value: unknown): value is Record<string, unknown> =>
  value !== null && typeof value === "object" && !Array.isArray(value);
const integerIn = (value: unknown, minimum: number, maximum: number): boolean =>
  Number.isInteger(value) && (value as number) >= minimum && (value as number) <= maximum;

function transcodeError(value: unknown): string | undefined {
  if (!plainObject(value)) return "distribution.transcode must be an object";
  const {images, audio, exclude, ...rest} = value;
  const extra = Object.keys(rest);
  if (extra.length > 0) return `distribution.transcode does not support ${extra.join(", ")}`;
  if (images === undefined && audio === undefined)
    return "distribution.transcode needs images, audio or both";
  if (images !== undefined) {
    if (!plainObject(images)) return "distribution.transcode.images must be an object";
    const {format, quality, effort, ...imageRest} = images;
    const imageExtra = Object.keys(imageRest);
    if (imageExtra.length > 0)
      return `distribution.transcode.images does not support ${imageExtra.join(", ")}`;
    if (format !== "webp") return "distribution.transcode.images.format must be webp";
    if (quality !== undefined && quality !== "lossless" && !integerIn(quality, 1, 100))
      return "distribution.transcode.images.quality must be an integer from 1 to 100 or \"lossless\"";
    if (effort !== undefined && !integerIn(effort, 0, 6))
      return "distribution.transcode.images.effort must be an integer from 0 to 6";
  }
  if (audio !== undefined) {
    if (!plainObject(audio)) return "distribution.transcode.audio must be an object";
    const {format, bitrate, quality, ...audioRest} = audio;
    const audioExtra = Object.keys(audioRest);
    if (audioExtra.length > 0)
      return `distribution.transcode.audio does not support ${audioExtra.join(", ")}`;
    if (format === "opus") {
      if (quality !== undefined)
        return "distribution.transcode.audio.quality applies to vorbis; opus takes bitrate";
      if (bitrate !== undefined && !integerIn(bitrate, 6, 256))
        return "distribution.transcode.audio.bitrate must be an integer from 6 to 256 (kbit/s)";
    } else if (format === "vorbis") {
      if (bitrate !== undefined)
        return "distribution.transcode.audio.bitrate applies to opus; vorbis takes quality";
      if (quality !== undefined && (typeof quality !== "number" || !Number.isFinite(quality) ||
          quality < -1 || quality > 10))
        return "distribution.transcode.audio.quality must be a number from -1 to 10";
    } else {
      return "distribution.transcode.audio.format must be opus or vorbis";
    }
  }
  if (exclude !== undefined && (!Array.isArray(exclude) || exclude.length > 256 ||
      !exclude.every((pattern) => typeof pattern === "string" && pattern.length > 0 &&
        pattern.length <= 512 && !pattern.includes("\\") && !pattern.startsWith("/") &&
        !pattern.includes(":") && !pattern.split("/").includes("..") &&
        !/[\u0000-\u001f]/.test(pattern))))
    return "distribution.transcode.exclude must be at most 256 project-relative glob patterns " +
      "using / separators";
  return undefined;
}

const plainText = (value: unknown, maxBytes: number): value is string =>
  typeof value === "string" && value.trim().length > 0 &&
  new TextEncoder().encode(value).length <= maxBytes && !/[\u0000-\u001f\u007f]/.test(value);
const environmentName = (value: unknown): boolean =>
  typeof value === "string" && /^[A-Za-z_][A-Za-z0-9_]{0,127}$/.test(value);

function androidDistributionError(value: unknown): string | undefined {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    return "distribution.android must be an object";
  const {applicationId, versionCode, versionName, label, icon, keystore, ...rest} =
    value as Record<string, unknown>;
  const extra = Object.keys(rest);
  if (extra.length > 0) return `distribution.android does not support ${extra.join(", ")}`;
  if (applicationId !== undefined && (typeof applicationId !== "string" ||
      applicationId.length > 150 ||
      !/^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+$/.test(applicationId)))
    return "distribution.android.applicationId must be a dotted Android package name such as com.example.game";
  if (versionCode !== undefined && (!Number.isInteger(versionCode) ||
      (versionCode as number) < 1 || (versionCode as number) > 2100000000))
    return "distribution.android.versionCode must be an integer from 1 to 2100000000";
  if (versionName !== undefined && !plainText(versionName, 100))
    return "distribution.android.versionName must be a single line of at most 100 bytes";
  if (label !== undefined && !plainText(label, 100))
    return "distribution.android.label must be a single line of at most 100 bytes";
  if (icon !== undefined && (typeof icon !== "string" || !isSafeResourcePath(icon) ||
      icon.includes(":") || !/\.png$/i.test(icon)))
    return "distribution.android.icon must be a safe project-relative .png path";
  if (keystore === undefined) return undefined;
  if (keystore === null || typeof keystore !== "object" || Array.isArray(keystore))
    return "distribution.android.keystore must be an object";
  const {path, alias, storePasswordEnv, keyPasswordEnv, ...keystoreRest} =
    keystore as Record<string, unknown>;
  const secrets = Object.keys(keystoreRest).filter((key) => /pass/i.test(key));
  if (secrets.length > 0)
    return `distribution.android.keystore must not contain ${secrets.join(", ")}: ` +
      "name environment variables in storePasswordEnv and keyPasswordEnv instead";
  const keystoreExtra = Object.keys(keystoreRest);
  if (keystoreExtra.length > 0)
    return `distribution.android.keystore does not support ${keystoreExtra.join(", ")}`;
  if (!plainText(path, 1024))
    return "distribution.android.keystore.path must be a keystore file path";
  if (!plainText(alias, 256))
    return "distribution.android.keystore.alias must name the signing key";
  if (!environmentName(storePasswordEnv))
    return "distribution.android.keystore.storePasswordEnv must be an environment variable name";
  if (keyPasswordEnv !== undefined && !environmentName(keyPasswordEnv))
    return "distribution.android.keystore.keyPasswordEnv must be an environment variable name";
  return undefined;
}

function issue(code: string, message: string, path: string, details?: string): AuthoringDiagnostic {
  return {
    severity: "error",
    code,
    message,
    path,
    ...(details === undefined ? {} : {details}),
  };
}

function object(value: unknown): ObjectValue | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as ObjectValue : undefined;
}

function sourceManifest(value: unknown, path: string): Result<ObjectValue> {
  if (value === undefined) return {value: {}, diagnostics: [], valid: true};
  if (typeof value === "string") {
    try {
      const parsed: unknown = JSON.parse(value);
      const record = object(parsed);
      return record === undefined
        ? {diagnostics: [issue("PXBUILD1040", "Project source manifest must be a JSON object", path)], valid: false}
        : {value: record, diagnostics: [], valid: true};
    } catch (error) {
      return {diagnostics: [issue("PXBUILD1040", "Project source manifest is not valid JSON", path, String(error))], valid: false};
    }
  }
  const record = object(value);
  return record === undefined
    ? {diagnostics: [issue("PXBUILD1040", "Project source manifest must be an object", path)], valid: false}
    : {value: record, diagnostics: [], valid: true};
}

function stableUuid(seed: string): string {
  const value = `${hash64(seed)}${hash64(`${seed}:tail`)}`.split("");
  value[12] = "5";
  value[16] = "8";
  const hex = value.join("");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20, 32)}`;
}

function identifier(name: unknown): string {
  if (typeof name !== "string") return "game";
  const result = name.toLowerCase().replace(/[^a-z0-9._-]+/gu, "-")
    .replace(/^[^a-z0-9]+/u, "").replace(/[^a-z0-9]+$/u, "").slice(0, 256);
  return result.length === 0 ? "game" : result;
}

function conventionalStoryPath(input: NormalizeProjectInput): string {
  if (input.storyPath !== undefined) return input.storyPath;
  if (input.sourceFiles?.["Story/main.pxstory"] !== undefined) return "Story/main.pxstory";
  const candidates = Object.keys(input.sourceFiles ?? {})
    .filter((path) => path.endsWith(".pxstory")).sort();
  return candidates.length === 1 ? candidates[0]! : "Story/main.pxstory";
}

/** Author-only character shorthand. Canonical .pxcharacter and .pxproject
 * artifacts still carry the UUIDs required by the native CharacterResources
 * loader; none of the shorthand fields enter those artifacts. */
function normalizeCharacterAuthoring(runtimeSource: ObjectValue,
  documents: Record<string, unknown>, projectId: string): readonly AuthoringDiagnostic[] {
  const descriptors = runtimeSource.characters;
  if (!Array.isArray(descriptors)) return [];
  const diagnostics: AuthoringDiagnostic[] = [];
  const assets: unknown[] = Array.isArray(runtimeSource.assets) ? [...runtimeSource.assets] : [];
  const assetsById = new Map<string, ObjectValue>();
  const assetsBySource = new Map<string, ObjectValue[]>();
  const rememberAsset = (value: ObjectValue): void => {
    if (typeof value.id === "string") assetsById.set(value.id.toLowerCase(), value);
    if (typeof value.source === "string") {
      const matches = assetsBySource.get(value.source) ?? [];
      matches.push(value);
      assetsBySource.set(value.source, matches);
    }
  };
  for (const asset of assets) {
    const record = object(asset);
    if (record !== undefined) rememberAsset(record);
  }
  const addAsset = (value: ObjectValue): void => {
    assets.push(value);
    rememberAsset(value);
  };
  let generatedAsset = false;
  const normalizedDescriptors = descriptors.map((value, characterIndex) => {
    const descriptor = object(value);
    if (descriptor === undefined || typeof descriptor.source !== "string") return value;
    const source = descriptor.source;
    if (!isSafeResourcePath(source) || source.includes(":") || source.includes("\0") ||
        source.split("/").some((part) => part === "" || part === ".")) return value;
    let authored = object(documents[source]);
    if (authored === undefined && typeof documents[source] === "string") {
      try { authored = object(JSON.parse(documents[source] as string) as unknown); }
      catch { /* The normal document parser reports malformed JSON below. */ }
    }
    const characterId = typeof descriptor.id === "string" ? descriptor.id
      : typeof authored?.id === "string" ? authored.id
        : stableUuid(`${projectId}:character:${source}`);
    const displayName = descriptor.displayName ?? authored?.displayName;
    const nextDescriptor = {...descriptor, id: characterId,
      ...(displayName === undefined ? {} : {displayName})};
    if (authored === undefined) return nextDescriptor;

    const nextCharacter: ObjectValue = {...authored,
      ...(authored.id === undefined ? {id: characterId} : {}),
      ...(authored.displayName === undefined && displayName !== undefined ? {displayName} : {})};
    if (nextCharacter.aliases === undefined) {
      const stem = source.split("/").at(-1)?.replace(/\.pxcharacter$/iu, "") ?? "";
      nextCharacter.aliases = [typeof descriptor.alias === "string" ? descriptor.alias
        : /[A-Za-z0-9]/u.test(stem) ? identifier(stem) : `character-${characterIndex + 1}`];
    }
    if (Array.isArray(authored.expressions)) {
      nextCharacter.expressions = authored.expressions.map((value, index) => {
        const expression = object(value);
        if (expression === undefined) return value;
        const nextExpression: ObjectValue = {...expression};
        const path = expression.assetPath;
        if (path !== undefined) {
          if (typeof path !== "string" || !isSafeResourcePath(path) ||
              path.includes(":") || path.includes("\0") ||
              path.split("/").some((part) => part === "" || part === ".")) {
            diagnostics.push(issue("PXBUILD1067",
              "Character expression assetPath must be a safe project-relative resource path",
              source, `expressions[${index}].assetPath`));
          } else if (expression.assetId !== undefined) {
            const pinnedId = expression.assetId;
            const existing = typeof pinnedId === "string"
              ? assetsById.get(pinnedId.toLowerCase()) : undefined;
            if (typeof pinnedId !== "string" ||
                existing !== undefined && (existing.source !== path ||
                  !["character", "cg", "uiImage"].includes(String(existing.kind)))) {
              diagnostics.push(issue("PXBUILD1068",
                "Character expression assetId conflicts with its assetPath or catalog entry",
                source, `expressions[${index}]`));
            } else if (existing === undefined) {
              addAsset({id: pinnedId, kind: "character", source: path});
              generatedAsset = true;
            }
          } else {
            const matching = (assetsBySource.get(path) ?? []).filter((asset) =>
              ["character", "cg", "uiImage"].includes(String(asset.kind)));
            if (matching.length > 1) {
              diagnostics.push(issue("PXBUILD1069",
                "Character expression assetPath matches multiple catalog assets; use an explicit assetId",
                source, `expressions[${index}].assetPath: ${path}`));
            } else if (matching.length === 1) {
              nextExpression.assetId = matching[0]!.id;
            } else {
              const assetId = stableUuid(`${projectId}:character-asset:${path}`);
              if (assetsById.has(assetId)) {
                diagnostics.push(issue("PXBUILD1070",
                  "Generated character assetId collides with an existing catalog entry; pin an explicit assetId",
                  source, `expressions[${index}].assetPath: ${path}`));
              } else {
                nextExpression.assetId = assetId;
                addAsset({id: assetId, kind: "character", source: path});
                generatedAsset = true;
              }
            }
          }
          delete nextExpression.assetPath;
        }
        // `facePath` lowers to `faceAssetId` the same way, reusing a cataloged image.
        const facePath = expression.facePath;
        if (facePath !== undefined) {
          if (typeof facePath !== "string" || !isSafeResourcePath(facePath) || facePath.includes(":")) {
            diagnostics.push(issue("PXBUILD1067",
              "Character expression facePath must be a safe project-relative resource path",
              source, `expressions[${index}].facePath`));
          } else if (expression.faceAssetId === undefined) {
            const matching = (assetsBySource.get(facePath) ?? []).filter((asset) =>
              ["character", "cg", "uiImage"].includes(String(asset.kind)));
            const faceAssetId = matching[0]?.id ??
              stableUuid(`${projectId}:character-face:${facePath}`);
            if (matching.length === 0) {
              addAsset({id: faceAssetId, kind: "character", source: facePath});
              generatedAsset = true;
            }
            nextExpression.faceAssetId = faceAssetId;
          }
          delete nextExpression.facePath;
        }
        if (nextExpression.id === undefined) {
          const seed = Array.isArray(expression.aliases) && typeof expression.aliases[0] === "string"
            ? expression.aliases[0] : typeof expression.name === "string" ? expression.name : String(index);
          nextExpression.id = stableUuid(`${projectId}:expression:${characterId}:${seed}`);
        }
        return nextExpression;
      });
      const expressions = nextCharacter.expressions as unknown[];
      if (nextCharacter.defaultExpressionId === undefined && expressions.length > 0)
        nextCharacter.defaultExpressionId = object(expressions[0])?.id;
      else if (typeof nextCharacter.defaultExpressionId === "string" &&
          !/^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/iu.test(nextCharacter.defaultExpressionId)) {
        const alias = nextCharacter.defaultExpressionId;
        const matched = expressions.map(object).find((expression) =>
          expression?.name === alias || Array.isArray(expression?.aliases) &&
          expression.aliases.includes(alias));
        if (matched !== undefined) nextCharacter.defaultExpressionId = matched.id;
      }
    }
    documents[source] = nextCharacter;
    return nextDescriptor;
  });
  runtimeSource.characters = normalizedDescriptors;
  if (generatedAsset || runtimeSource.assets === undefined) runtimeSource.assets = assets;
  return diagnostics;
}

/** Canonical Gallery references carry their own stable asset identity and
 * source path. Promote them to project assets so native catalog validation and
 * runtime lookup see the same identity without duplicating author metadata. */
function normalizeGalleryAssets(runtimeSource: ObjectValue,
  documents: Record<string, unknown>, catalogPath: string): readonly AuthoringDiagnostic[] {
  let catalog = object(documents[catalogPath]);
  if (catalog === undefined && typeof documents[catalogPath] === "string") {
    try { catalog = object(JSON.parse(documents[catalogPath] as string) as unknown); }
    catch { /* The document validator reports invalid JSON below. */ }
  }
  if (!Array.isArray(catalog?.gallery)) return [];
  if (runtimeSource.assets !== undefined && !Array.isArray(runtimeSource.assets))
    return []; // Project schema validation reports the malformed asset list.
  const assets = Array.isArray(runtimeSource.assets) ? [...runtimeSource.assets] : [];
  const byId = new Map<string, ObjectValue>();
  const bySource = new Map<string, ObjectValue>();
  for (const value of assets) {
    const asset = object(value);
    if (asset === undefined) continue;
    if (typeof asset.id === "string") byId.set(asset.id.toLowerCase(), asset);
    if (typeof asset.source === "string") bySource.set(asset.source, asset);
  }
  const diagnostics: AuthoringDiagnostic[] = [];
  for (const [index, value] of catalog.gallery.entries()) {
    const item = object(value);
    if (item === undefined) continue;
    for (const property of ["image", "thumbnail"] as const) {
      const reference = object(item[property]);
      const id = reference?.id;
      const source = reference?.path;
      const kind = reference?.kind;
      if (typeof id !== "string" || typeof source !== "string" ||
          typeof kind !== "string") continue;
      const existingId = byId.get(id.toLowerCase());
      const existingSource = bySource.get(source);
      if (existingId !== undefined || existingSource !== undefined) {
        if (existingId?.source !== source ||
            existingSource !== undefined &&
            (typeof existingSource.id !== "string" ||
             existingSource.id.toLowerCase() !== id.toLowerCase())) {
          diagnostics.push(issue("PXBUILD1071",
            "Gallery asset identity conflicts with an existing project asset",
            catalogPath, `gallery[${index}].${property}: ${id} / ${source}`));
        }
        continue;
      }
      const descriptor = {id, source, kind};
      assets.push(descriptor);
      byId.set(id.toLowerCase(), descriptor);
      bySource.set(source, descriptor);
    }
  }
  if (diagnostics.length === 0) runtimeSource.assets = assets;
  return diagnostics;
}

function defaultUi(projectId: string, name: string, width: number, height: number): unknown {
  const sceneId = stableUuid(`${projectId}:default-ui:scene`);
  const rootId = stableUuid(`${projectId}:default-ui:root`);
  const titleId = stableUuid(`${projectId}:default-ui:title`);
  const startId = stableUuid(`${projectId}:default-ui:start`);
  const layout = (x: number, y: number, nodeWidth: number, nodeHeight: number) => ({
    mode: "free", x, y, width: nodeWidth, height: nodeHeight,
    anchorX: 0, anchorY: 0, anchorRight: 0, anchorBottom: 0,
    pivotX: 0, pivotY: 0, margin: 0, alignment: "start", sizeRule: "fixed",
  });
  return {
    format: "PrismatiXUIScene",
    schemaRevision: 2,
    id: sceneId,
    revision: 1,
    name,
    width,
    height,
    rootId,
    nodes: [{
      id: rootId,
      parentId: null,
      order: 0,
      kind: "control",
      runtimeType: "Control",
      name: "Root",
      visible: true,
      locked: false,
      layout: layout(0, 0, width, height),
      runtimeProperties: {},
      bindings: {},
    }, {
      id: titleId,
      parentId: rootId,
      order: 0,
      kind: "label",
      runtimeType: "Label",
      name: "GameTitle",
      visible: true,
      locked: false,
      layout: layout(Math.round(width * 0.1), Math.round(height * 0.16),
        Math.round(width * 0.8), 96),
      text: name,
      runtimeProperties: {fontSize: 52},
      bindings: {},
    }, {
      id: startId,
      parentId: rootId,
      order: 1,
      kind: "button",
      runtimeType: "Button",
      name: "Start",
      visible: true,
      locked: false,
      layout: layout(Math.round((width - 300) / 2), Math.round(height * 0.55), 300, 64),
      text: "Start",
      onClick: {id: "game.start", arguments: {}},
      accessibilityLabel: "Start game",
      accessibilityRole: "button",
      runtimeProperties: {},
      bindings: {},
    }],
    theme: [],
    behaviorGraph: {nodes: [], links: [], groups: []},
    behaviorTriggers: [],
    visualStateGroups: [],
  };
}

/**
 * Expands the beginner-facing single-locale project shape into the unchanged,
 * strict runtime contracts. Explicit advanced configuration always wins.
 * Generated documents are returned in-memory so callers may preview/build a
 * project containing only `Story/main.pxstory` and an optional small manifest.
 */
export function normalizeProject(input: NormalizeProjectInput = {}): Result<NormalizedProjectInput> {
  const projectPath = input.projectPath ?? "project.pxproject";
  const parsedSource = sourceManifest(input.project, projectPath);
  if (!parsedSource.valid || parsedSource.value === undefined)
    return {diagnostics: parsedSource.diagnostics, valid: false};

  const raw = parsedSource.value;
  const includeValue = raw.resourceIncludes;
  if (includeValue !== undefined && (!Array.isArray(includeValue) ||
      includeValue.length > 4096 || !includeValue.every((value: unknown) =>
        typeof value === "string" && isSafeResourcePath(value) &&
        !value.includes(":") && !value.includes("\0") &&
        ![".prismatix", ".git", "node_modules"].includes(value.split("/")[0]!.toLowerCase()) &&
        value.split("/").every((part) => part.length > 0 && part !== "."))))
    return {diagnostics: [issue("PXBUILD1051",
      "resourceIncludes must be an array of safe project-relative file or directory paths", projectPath)],
    valid: false};
  const resourceIncludes = includeValue as readonly string[] | undefined;
  const storyAssetsError = raw.storyAssets === undefined ? undefined
    : storyAssetRootsError(raw.storyAssets);
  if (storyAssetsError !== undefined)
    return {diagnostics: [issue("PXBUILD1056", storyAssetsError, projectPath)], valid: false};
  const storyAssets = raw.storyAssets as StoryAssetRoots | undefined;
  const storyMacrosError = raw.storyMacros === undefined ? undefined
    : storyMacroFilesError(raw.storyMacros);
  if (storyMacrosError !== undefined)
    return {diagnostics: [issue("PXBUILD1120", storyMacrosError, projectPath)], valid: false};
  const storyMacros = raw.storyMacros as readonly string[] | undefined;
  const distributionError = raw.distribution === undefined ? undefined
    : projectDistributionError(raw.distribution);
  if (distributionError !== undefined)
    return {diagnostics: [issue("PXBUILD1065", distributionError, projectPath)], valid: false};
  const distribution = raw.distribution as ProjectDistribution | undefined;
  const runtimeSource = {...raw};
  delete runtimeSource.resourceIncludes;
  delete runtimeSource.storyAssets;
  delete runtimeSource.storyMacros;
  delete runtimeSource.distribution;
  const name = typeof raw.name === "string" ? raw.name : "PrismatiX Game";
  const projectId = typeof raw.id === "string" ? raw.id : identifier(name);
  const documents = Object.assign(Object.create(null) as Record<string, unknown>,
    input.documents);
  const characterDiagnostics = normalizeCharacterAuthoring(runtimeSource,
    documents, projectId);
  if (characterDiagnostics.length > 0)
    return {diagnostics: characterDiagnostics, valid: false};
  const locale = typeof raw.defaultLocale === "string" ? raw.defaultLocale
    : input.defaultLocale ?? "en-US";
  const supportedLocales = raw.supportedLocales ?? [locale];
  const resolution = object(raw.resolution) ?? {};
  const width = typeof resolution.width === "number" ? resolution.width : 1280;
  const height = typeof resolution.height === "number" ? resolution.height : 720;
  const storyIndexPath = typeof raw.storyIndex === "string"
    ? raw.storyIndex : "Story/story.pxindex";
  const gameCatalogPath = typeof raw.gameCatalog === "string"
    ? raw.gameCatalog : "Content/game.pxgame";
  const galleryDiagnostics = normalizeGalleryAssets(runtimeSource,
    documents, gameCatalogPath);
  if (galleryDiagnostics.length > 0)
    return {diagnostics: galleryDiagnostics, valid: false};
  const uiEntryPoints = object(raw.uiEntryPoints);
  const defaultUiPath = "Content/UI/Title.pxui";
  const defaultUiRoute = uiEntryPoints === undefined
    ? "title" : Object.keys(uiEntryPoints).sort()[0] ?? "title";
  const entry = object(raw.entry) ?? {};
  const explicitStoryIndex = object(input.documents?.[storyIndexPath]);
  const entryStory = typeof entry.story === "string" ? entry.story
    : typeof explicitStoryIndex?.entryScene === "string" ? explicitStoryIndex.entryScene : "main";
  const storyPath = conventionalStoryPath(input);

  const candidate: unknown = {
    format: "PrismatiXProject",
    schemaRevision: 2,
    id: projectId,
    name,
    version: "0.1.0",
    contentVersion: "1",
    saveVersion: 1,
    defaultLocale: locale,
    supportedLocales,
    storyIndex: storyIndexPath,
    gameCatalog: gameCatalogPath,
    extensions: [],
    assets: [],
    uiEntryPoints: uiEntryPoints ?? {[defaultUiRoute]: defaultUiPath},
    ...runtimeSource,
    resolution: {...{width, height}, ...resolution},
    entry: {
      story: entryStory,
      ui: typeof entry.ui === "string" ? entry.ui : defaultUiRoute,
    },
  };
  const validated = validateDocument<ProjectDocument>("project", candidate, projectPath);
  if (!validated.valid || validated.value === undefined)
    return {diagnostics: validated.diagnostics, valid: false};

  const project = validated.value;
  // Player's JSON-to-Variant adapter has a finite depth. Reject an authored
  // value here instead of publishing a package which fails only at boot.
  const convertibleUiData = (value: unknown, depth: number): boolean => {
    if (depth > 32) return false;
    if (Array.isArray(value)) return value.every((item) =>
      convertibleUiData(item, depth + 1));
    if (value !== null && typeof value === "object") return Object.values(value).every((item) =>
      convertibleUiData(item, depth + 1));
    if (typeof value === "number") return Number.isFinite(value) &&
      (!Number.isInteger(value) || Number.isSafeInteger(value));
    return value === null || typeof value === "string" || typeof value === "boolean";
  };
  const uiData = object(project.settings?.["player.uiData"]);
  if (uiData !== undefined && !Object.values(uiData).every((value) =>
    convertibleUiData(value, 0)))
    return {diagnostics: [issue("PXBUILD1059",
      "player.uiData contains a value too deep or numerically unsafe for Player",
      projectPath, "settings.player.uiData")], valid: false};
  const locales: Record<string, unknown> = {...input.locales};
  if (raw.storyIndex === undefined && documents[project.storyIndex] === undefined) {
    documents[project.storyIndex] = {
      format: "PrismatiXStoryIndex",
      schemaRevision: 2,
      id: "main",
      entryScene: project.entry.story,
      chapters: [{id: "main", title: name, scenes: [project.entry.story]}],
      scenes: [{id: project.entry.story, sources: {[project.defaultLocale]: storyPath}}],
    };
  }
  if (raw.gameCatalog === undefined && documents[project.gameCatalog] === undefined) {
    documents[project.gameCatalog] = {
      format: "PrismatiXGame", schemaRevision: 2,
      variables: [], gallery: [], unlockables: [],
    };
  }
  if (raw.uiEntryPoints === undefined && documents[defaultUiPath] === undefined) {
    documents[defaultUiPath] = defaultUi(project.id, name,
      project.resolution.width, project.resolution.height);
  }
  if (project.supportedLocales.length === 1 && locales[project.defaultLocale] === undefined) {
    locales[project.defaultLocale] = {
      format: "PrismatiXLocale", schemaRevision: 2,
      locale: project.defaultLocale, strings: {},
    };
  }

  return {
    value: {
      project,
      documents,
      sourceFiles: input.sourceFiles ?? {},
      locales,
      projectPath,
      storyPath,
      resourceIncludes: resourceIncludes ?? [],
      ...(storyAssets === undefined ? {} : {storyAssets}),
      ...(storyMacros === undefined ? {} : {storyMacros}),
      ...(distribution === undefined ? {} : {distribution}),
    },
    diagnostics: validated.diagnostics,
    valid: true,
  };
}
