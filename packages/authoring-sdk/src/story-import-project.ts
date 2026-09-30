import {canonicalJson} from "./canonical-json.js";
import {contractDocumentByteLimit, validateDocument} from "./contracts.js";
import {isSafeResourcePath} from "./resources.js";
import {buildImportedStorySource, type ImportedStoryLine,
  type ImportedStorySourceOptions} from "./story-import.js";
import {serializeStoryIdentityMap, type StoryIdentityMap} from "./story-external-identity.js";
import type {StoryIndexDocument} from "./story-project.js";
import type {AuthoringDiagnostic, Result} from "./types.js";

export interface ImportedStoryScene {
  readonly id: string;
  readonly chapter: string;
  readonly title?: string;
  readonly identityNamespace?: string;
  readonly requiredLabels?: readonly string[];
  readonly sources: Readonly<Record<string, {
    readonly path: string;
    readonly lines: readonly ImportedStoryLine[];
    readonly options?: ImportedStorySourceOptions;
  }>>;
}

export interface ImportedStoryProjectInput {
  readonly id: string;
  readonly entryScene: string;
  readonly locales: readonly string[];
  readonly chapters: readonly {readonly id: string; readonly title: string}[];
  readonly scenes: readonly ImportedStoryScene[];
  readonly indexPath?: string;
}

export interface ImportedStoryProject {
  readonly storyIndex: StoryIndexDocument;
  /** Canonical index and source/sidecar texts, keyed by project-relative path. */
  readonly files: Readonly<Record<string, string>>;
}

function issue(code: string, message: string, path?: string,
  details?: string): AuthoringDiagnostic {
  return {severity: "error", code, message,
    ...(path === undefined ? {} : {path}),
    ...(details === undefined ? {} : {details})};
}

function identities(map: StoryIdentityMap): string {
  return JSON.stringify(map.entries.map((entry) =>
    [entry.id, entry.kind, entry.command ?? null]));
}

/** Assemble already-decoded scenes into SDK Story inputs. Format-specific
 * adapters own source decoding and command translation; this pure step owns
 * path collisions, every-locale coverage, save IDs and sidecar publication.
 * Call compileStoryProject/compileProject afterwards for story semantics. */
export function assembleImportedStoryProject(
  input: ImportedStoryProjectInput): Result<ImportedStoryProject> {
  if (input === null || typeof input !== "object" ||
      !Array.isArray(input.locales) || !Array.isArray(input.chapters) ||
      !Array.isArray(input.scenes) ||
      !input.locales.every((locale) => typeof locale === "string") ||
      !input.chapters.every((chapter) => chapter !== null && typeof chapter === "object") ||
      !input.scenes.every((scene) => scene !== null && typeof scene === "object"))
    return {valid: false, diagnostics: [issue("PXIMPORT1020",
      "Imported Story project must declare locales, chapters and scenes")]};
  const indexPath = input.indexPath ?? "Story/story.pxindex";
  const diagnostics: AuthoringDiagnostic[] = [];
  if (typeof indexPath !== "string" || !isSafeResourcePath(indexPath) || !indexPath.endsWith(".pxindex") ||
      indexPath.includes(":") || /[\u0000-\u001f\u007f]/u.test(indexPath) ||
      indexPath.split("/").some((part) => part.length === 0 || part === "."))
    diagnostics.push(issue("PXIMPORT1018",
      "Imported Story index must use a safe project-relative .pxindex path", indexPath));
  const locales = new Set(input.locales);
  if (locales.size === 0 || locales.size !== input.locales.length)
    diagnostics.push(issue("PXIMPORT1011", "Imported locales must be nonempty and unique", indexPath));
  const chapters = input.chapters.map((chapter) => ({
    id: chapter.id, title: chapter.title,
    scenes: input.scenes.filter((scene) => scene.chapter === chapter.id)
      .map((scene) => scene.id),
  }));
  const chapterIds = new Set(input.chapters.map((chapter) => chapter.id));
  if (chapterIds.size !== input.chapters.length)
    diagnostics.push(issue("PXIMPORT1012", "Imported chapter IDs must be unique", indexPath));
  const files: Record<string, string> = Object.create(null) as Record<string, string>;
  const pathOwners = new Map<string, string>();
  const namespaceIds = new Map<string, Set<string>>();
  const scenes: StoryIndexDocument["scenes"][number][] = [];
  for (const scene of input.scenes) {
    if (scene === null || typeof scene !== "object" ||
        scene.sources === null || typeof scene.sources !== "object" ||
        Array.isArray(scene.sources)) {
      diagnostics.push(issue("PXIMPORT1020", "Imported scene must declare locale sources",
        indexPath));
      continue;
    }
    if (!chapterIds.has(scene.chapter))
      diagnostics.push(issue("PXIMPORT1013", "Imported scene refers to an unknown chapter",
        indexPath, scene.id));
    const sources: Record<string, string> = Object.create(null) as Record<string, string>;
    let referenceIds: string | undefined;
    for (const locale of input.locales) {
      const source = scene.sources[locale];
      if (source === undefined) {
        diagnostics.push(issue("PXIMPORT1014", "Imported scene is missing a locale",
          indexPath, `${scene.id}: ${locale}`));
        continue;
      }
      if (source === null || typeof source !== "object") {
        diagnostics.push(issue("PXIMPORT1020", "Imported locale source is invalid",
          indexPath, `${scene.id}: ${locale}`));
        continue;
      }
      const built = buildImportedStorySource(source.path, source.lines,
        source.options);
      if (!built.valid || built.value === undefined) {
        diagnostics.push(...built.diagnostics);
        continue;
      }
      const currentIds = identities(built.value.identityMap);
      if (referenceIds !== undefined && currentIds !== referenceIds)
        diagnostics.push(issue("PXIMPORT1010",
          "Imported scene has different execution identities across locales",
          source.path, scene.id));
      else if (referenceIds === undefined) referenceIds = currentIds;
      const namespace = scene.identityNamespace ?? scene.id;
      if (locale === input.locales[0]) {
        const used = namespaceIds.get(namespace) ?? new Set<string>();
        for (const entry of built.value.identityMap.entries) {
          if (entry.id === undefined) continue;
          if (used.has(entry.id)) diagnostics.push(issue("PXIMPORT1015",
            "Imported execution ID is reused within its identity namespace",
            source.path, entry.id));
          used.add(entry.id);
        }
        namespaceIds.set(namespace, used);
      }
      for (const path of [source.path, `${source.path}.pxids.json`]) {
        const key = path.toLocaleLowerCase("en-US");
        const owner = pathOwners.get(key);
        if (owner !== undefined) diagnostics.push(issue("PXIMPORT1016",
          "Imported source path collides with another scene or locale", path, owner));
        else pathOwners.set(key, `${scene.id}: ${locale}`);
      }
      sources[locale] = source.path;
      files[source.path] = built.value.source;
      files[`${source.path}.pxids.json`] = serializeStoryIdentityMap(built.value.identityMap);
    }
    for (const locale of Object.keys(scene.sources)) if (!locales.has(locale))
      diagnostics.push(issue("PXIMPORT1017", "Imported scene declares an unknown locale",
        indexPath, `${scene.id}: ${locale}`));
    scenes.push({id: scene.id, ...(scene.title === undefined ? {} : {title: scene.title}),
      ...(scene.identityNamespace === undefined ? {} : {identityNamespace: scene.identityNamespace}),
      sources,
      ...(scene.requiredLabels === undefined ? {} : {requiredLabels: scene.requiredLabels})});
  }
  const storyIndex: StoryIndexDocument = {format: "PrismatiXStoryIndex",
    schemaRevision: 2, id: input.id, entryScene: input.entryScene, chapters,
    scenes};
  const validated = validateDocument<StoryIndexDocument>("storyIndex", storyIndex, indexPath);
  diagnostics.push(...validated.diagnostics);
  if (diagnostics.length > 0) return {valid: false, diagnostics};
  const indexText = canonicalJson(storyIndex);
  if (Buffer.byteLength(indexText, "utf8") > contractDocumentByteLimit("storyIndex"))
    diagnostics.push(issue("PXIMPORT1019",
      "Imported Story index exceeds the 16 MiB document limit", indexPath));
  if (diagnostics.length > 0) return {valid: false, diagnostics};
  files[indexPath] = indexText;
  return {valid: true, diagnostics: [], value: {storyIndex, files}};
}
