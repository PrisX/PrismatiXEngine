import {readFile} from "node:fs/promises";
import {Worker} from "node:worker_threads";

import type {AuthoringDiagnostic} from "@prismatix/authoring-sdk";

import {ProjectModel, withoutRewriteEchoes} from "./project.js";
import type {SdkMode} from "./sdk.js";

export interface ProjectValidationRequest {
  readonly root: string;
  readonly sdkMode: SdkMode;
  /** Unsaved editor text keyed by project-relative Story path. */
  readonly overlays: Readonly<Record<string, string>>;
}

export interface ProjectValidationResult {
  readonly root: string;
  readonly diagnostics: readonly AuthoringDiagnostic[];
  /** Project-relative Story sources that were compiled. */
  readonly sources: readonly string[];
  readonly durationMs: number;
}

/** Compiles every Story scene of every supported locale with the SDK's
 * compileStoryProject, as `prismatix validate` does for Story, and checks
 * that resources referenced by Story exist. UI and packaging checks remain
 * the CLI's job. */
export async function validateProjectStories(request: ProjectValidationRequest): Promise<ProjectValidationResult> {
  const started = performance.now();
  const model = await ProjectModel.load(request.root, request.sdkMode);
  const diagnostics: AuthoringDiagnostic[] = [...model.loadDiagnostics];
  const sources: string[] = [];
  if (model.valid && model.project !== undefined) {
    const locales = new Set([model.project.defaultLocale, ...model.project.supportedLocales]);
    for (const locale of locales) {
      const sourceFiles: Record<string, string> = Object.create(null) as Record<string, string>;
      const scenes = model.sceneSources(locale);
      if (scenes.length === 0) continue;
      for (const scene of scenes) {
        if (Object.hasOwn(sourceFiles, scene.path)) continue;
        const overlay = request.overlays[scene.path];
        try {
          sourceFiles[scene.path] = overlay ?? await readFile(model.toFsPath(scene.path), "utf8");
          sources.push(scene.path);
        } catch { continue; }
        try {
          sourceFiles[`${scene.path}.pxids.json`] = await readFile(model.toFsPath(`${scene.path}.pxids.json`), "utf8");
        } catch { /* sidecars are optional unless the Story's marker requires one */ }
      }
      const result = model.sdkApi.compileStoryProject(model.compileContext(locale, sourceFiles));
      diagnostics.push(...withoutRewriteEchoes(result.diagnostics));
      for (const location of result.resourceLocations ?? [])
        if (!model.fileExists(location.source))
          diagnostics.push({severity: "error", code: "PXBUILD1044",
            message: `Project dependency source is missing: ${location.source}`,
            path: location.span.path, span: location.span});
    }
  }
  const seen = new Set<string>();
  const unique = diagnostics.filter((diagnostic) => {
    const key = JSON.stringify([diagnostic.code, diagnostic.message, diagnostic.span ?? diagnostic.path]);
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  });
  return {root: model.root, diagnostics: unique, sources, durationMs: Math.round(performance.now() - started)};
}

/** Runs {@link validateProjectStories} on a worker thread so a large project
 * never blocks completion or hover, and its memory is released afterwards. */
export function validateProjectInWorker(request: ProjectValidationRequest): Promise<ProjectValidationResult> {
  return new Promise((resolvePromise, reject) => {
    const worker = new Worker(new URL("./project-worker.js", import.meta.url), {workerData: request});
    let settled = false;
    worker.once("message", (message: {result?: ProjectValidationResult; error?: string}) => {
      settled = true;
      if (message.result !== undefined) resolvePromise(message.result);
      else reject(new Error(message.error ?? "project validation failed"));
      void worker.terminate();
    });
    worker.once("error", (error) => { if (!settled) { settled = true; reject(error); } });
    worker.once("exit", (code) => {
      if (!settled) { settled = true; reject(new Error(`project validation worker exited with ${code}`)); }
    });
  });
}
