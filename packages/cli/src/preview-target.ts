import type {BuildArtifact} from "@prismatix/authoring-sdk";

/**
 * Resolves `prismatix dev --at` to the Runtime IR operation Preview seeks to.
 * Accepted forms: `<scene>`, `<scene>:<line>`, `<scene>:<label>` and
 * `<Story/.../file.pxstory>:<line>`, all in the default locale.
 */
export interface PreviewTarget {
  readonly operationIndex: number;
  readonly sourceUri: string;
  readonly line: number;
}

function sceneSource(artifact: BuildArtifact, scene: string): string {
  const text = artifact.files[artifact.project.storyIndex];
  if (text === undefined) {
    // A single-story project has no index; its only scene is the entry.
    const story = Object.keys(artifact.files).find((path) => path.endsWith(".pxstory"));
    if (story !== undefined && (scene === artifact.project.entry.story || scene === "main")) return story;
    throw new Error(`Unknown scene ${scene}`);
  }
  const index = JSON.parse(text) as {scenes?: {id: string; sources: Record<string, string>}[]};
  const entry = index.scenes?.find((candidate) => candidate.id === scene);
  if (entry === undefined) throw new Error(`The story index has no scene ${scene}`);
  const source = entry.sources[artifact.project.defaultLocale] ?? Object.values(entry.sources)[0];
  if (source === undefined) throw new Error(`Scene ${scene} has no source`);
  return source;
}

export function resolvePreviewTarget(artifact: BuildArtifact, target: string): PreviewTarget {
  const separator = target.lastIndexOf(":");
  const head = separator < 0 ? target : target.slice(0, separator);
  const tail = separator < 0 ? undefined : target.slice(separator + 1);
  const sourceUri = head.endsWith(".pxstory") ? head.replaceAll("\\", "/") : sceneSource(artifact, head);
  let line = 1;
  if (tail !== undefined && /^\d+$/u.test(tail)) line = Number(tail);
  else if (tail !== undefined && tail.length > 0) {
    const text = artifact.files[sourceUri];
    if (text === undefined) throw new Error(`${sourceUri} is not part of this build`);
    const found = text.split(/\r?\n/u).findIndex((value) =>
      new RegExp(`^\\*${tail.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&")}(?:\\s|$)`, "u").test(value));
    if (found < 0) throw new Error(`${sourceUri} has no label *${tail}`);
    line = found + 1;
  }
  const lines = new Map(artifact.sourceMap.mappings
    .filter((mapping) => mapping.sourceUri === sourceUri)
    .map((mapping) => [mapping.operationId, mapping.startLine]));
  if (lines.size === 0) throw new Error(`${sourceUri} has no compiled Story operations`);
  let best: {index: number; line: number} | undefined;
  artifact.runtimeIr.operations.forEach((operation, index) => {
    const start = lines.get(operation.operationId);
    if (start === undefined || start < line) return;
    if (best === undefined || start < best.line) best = {index, line: start};
  });
  if (best === undefined) throw new Error(`${sourceUri} has no Story operation at or after line ${line}`);
  const found = best as {index: number; line: number};
  return {operationIndex: found.index, sourceUri, line: found.line};
}
