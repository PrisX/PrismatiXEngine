// The parts of a .model3.json this sample reads. Everything is resolved
// against the folder of the model3.json, which is a project resource path.

export interface MotionFile {
  readonly file: string;
  readonly fadeIn?: number;
  readonly fadeOut?: number;
}

export interface Model3 {
  /** Project path of the .moc3 (the sample reads `<moc>.b64`, see tools/moc-to-text.mjs). */
  readonly moc: string;
  readonly textures: readonly string[];
  readonly motions: Readonly<Record<string, readonly MotionFile[]>>;
  readonly expressions: Readonly<Record<string, string>>;
  readonly eyeBlinkIds: readonly string[];
  readonly lipSyncIds: readonly string[];
}

/** "a/b/../c.png" -> "a/c.png"; `base` is a folder, `relative` may climb out of it. */
export function resolvePath(base: string, relative: string): string {
  const parts = base === "" ? [] : base.split("/").filter((part) => part !== "");
  for (const part of relative.split("/")) {
    if (part === "" || part === ".") continue;
    if (part === "..") parts.pop();
    else parts.push(part);
  }
  return parts.join("/");
}

export function folderOf(path: string): string {
  const slash = path.lastIndexOf("/");
  return slash < 0 ? "" : path.slice(0, slash);
}

interface RawModel3 {
  FileReferences?: {
    Moc?: string;
    Textures?: string[];
    Motions?: Record<string, {File: string; FadeInTime?: number; FadeOutTime?: number}[]>;
    Expressions?: {Name: string; File: string}[];
  };
  Groups?: {Target?: string; Name?: string; Ids?: string[]}[];
}

export function parseModel3(text: string, modelPath: string): Model3 {
  const json = JSON.parse(text) as RawModel3;
  const references = json.FileReferences;
  if (!references?.Moc) throw new Error(`Live2D: ${modelPath} has no FileReferences.Moc`);
  const folder = folderOf(modelPath);
  const motions: Record<string, MotionFile[]> = {};
  for (const [group, entries] of Object.entries(references.Motions ?? {}))
    motions[group] = entries.map((entry) => ({
      file: resolvePath(folder, entry.File),
      ...(entry.FadeInTime === undefined ? {} : {fadeIn: entry.FadeInTime}),
      ...(entry.FadeOutTime === undefined ? {} : {fadeOut: entry.FadeOutTime}),
    }));
  const expressions: Record<string, string> = {};
  for (const entry of references.Expressions ?? []) expressions[entry.Name] = resolvePath(folder, entry.File);
  const group = (name: string): string[] =>
    (json.Groups ?? []).filter((entry) => entry.Target === "Parameter" && entry.Name === name).flatMap((entry) => entry.Ids ?? []);
  return {
    moc: resolvePath(folder, references.Moc),
    textures: (references.Textures ?? []).map((texture) => resolvePath(folder, texture)),
    motions,
    expressions,
    eyeBlinkIds: group("EyeBlink"),
    lipSyncIds: group("LipSync"),
  };
}
