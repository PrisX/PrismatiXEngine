import {readFile} from "node:fs/promises";
import {resolve} from "node:path";

import {compileProject} from "@prismatix/authoring-sdk";

const sampleRoot = resolve("Samples/CommercialVN");
const json = async (path: string): Promise<unknown> =>
  JSON.parse(await readFile(resolve(sampleRoot, path), "utf8")) as unknown;
const record = (value: unknown): Record<string, unknown> => {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new Error("Expected a canonical object document");
  return value as Record<string, unknown>;
};

const project = await json("project.pxproject");
const descriptor = record(project);
const documents: Record<string, unknown> = {};
const sourceFiles: Record<string, string> = {};
const locales: Record<string, unknown> = {};
const loadDocument = async (path: unknown): Promise<void> => {
  if (typeof path !== "string" || documents[path] !== undefined) return;
  documents[path] = await json(path);
};

await loadDocument(descriptor.storyIndex);
await loadDocument(descriptor.gameCatalog);
for (const path of Object.values(record(descriptor.uiEntryPoints)))
  await loadDocument(path);
for (const collection of ["extensions", "characters", "uiComponents",
  "saveMigrations", "effects"] as const) {
  for (const item of Array.isArray(descriptor[collection])
    ? descriptor[collection] as unknown[] : []) {
    if (typeof item === "string") await loadDocument(item);
    else {
      const value = record(item);
      await loadDocument(value.source ?? value.asset);
    }
  }
}

const storyIndex = record(documents[String(descriptor.storyIndex)]);
for (const scene of Array.isArray(storyIndex.scenes)
  ? storyIndex.scenes as unknown[] : []) {
  for (const path of Object.values(record(record(scene).sources))) {
    if (typeof path === "string")
      sourceFiles[path] = await readFile(resolve(sampleRoot, path), "utf8");
  }
}
for (const item of Array.isArray(descriptor.effects)
  ? descriptor.effects as unknown[] : []) {
  const effectPath = record(item).source;
  if (typeof effectPath !== "string") continue;
  const shader = record(documents[effectPath]).shader;
  if (typeof shader === "string")
    sourceFiles[shader] = await readFile(resolve(sampleRoot, shader), "utf8");
}
for (const locale of Array.isArray(descriptor.supportedLocales)
  ? descriptor.supportedLocales as unknown[] : []) {
  if (typeof locale === "string")
    locales[locale] = await json(`Content/Localization/${locale}.json`);
}

const result = compileProject({project, documents, sourceFiles, locales,
  projectPath: "project.pxproject"});
if (!result.valid || result.artifact === undefined)
  throw new Error(`Commercial sample did not compile: ${JSON.stringify(result.diagnostics)}`);
if (result.artifact.engineVersion !== "0.2.0" ||
    result.artifact.sourceMap.mappings.length === 0 ||
    result.artifact.localeArtifacts["ja-JP"] === undefined)
  throw new Error("BuildArtifact omitted its version, source map, or locale programs");

process.stdout.write("Authoring SDK documentation example passed\n");
