import type {AuthoringDiagnostic} from "@prismatix/authoring-sdk";

import {importArtemis} from "./artemis/index.js";
import {ArtemisConfigError} from "./artemis/config.js";
import {ArtemisSyntaxError} from "./artemis/ast.js";
import {ImportFailure} from "./publish.js";

/**
 * `prismatix import <format> <source> [project]`. Each format adapter decodes
 * its own source files and hands translated scenes to publishImportedStory;
 * a further format (KiriKiri, Ren'Py, ...) registers beside `artemis` here.
 */
export interface ImportCommandOptions {
  readonly values: Map<string, string>;
  readonly flags: Set<string>;
  readonly positional: string[];
}

interface Importer {
  readonly run: (source: string, project: string, options: ImportCommandOptions,
    log: (message: string) => void) => Promise<{readonly summary: object; readonly lines: readonly string[]}>;
}

const importers: Readonly<Record<string, Importer>> = {
  artemis: {
    async run(source, project, options, log) {
      const summary = await importArtemis({extracted: source, project,
        ...(options.values.has("config") ? {config: options.values.get("config")!} : {}),
        longForms: options.flags.has("long-forms"), log});
      return {summary, lines: [
        `Imported ${summary.scenes} scenes: ${summary.dialogue} text blocks, ${summary.voices} voices, ` +
          `${summary.characters} characters, ${summary.expressions} expressions.`,
        `Story: ${summary.storyFilesWritten} of ${summary.storyFiles} files changed` +
          (summary.preservedIdentities > 0 ? `; ${summary.preservedIdentities} sources kept their earlier IDs` : "") +
          (summary.removedPortraitFiles > 0 ? `; removed ${summary.removedPortraitFiles} stale portrait files` : "") + ".",
        ...(summary.regeneratedIdentities.length > 0 ? [`Re-identified (saves inside may not resume): ${
          summary.regeneratedIdentities.slice(0, 10).join(", ")}${summary.regeneratedIdentities.length > 10 ? ", ..." : ""}`] : []),
        `${summary.unsupported} unsupported Artemis features stay as comments; see ${summary.report}.`,
      ]};
    },
  },
};

export const importFormats = Object.keys(importers);

function diagnostic(code: string, message: string, path?: string, details?: string): AuthoringDiagnostic {
  return {severity: "error", code, message, ...(path === undefined ? {} : {path}),
    ...(details === undefined ? {} : {details})};
}

function print(json: boolean, values: readonly AuthoringDiagnostic[]): void {
  for (const value of values) {
    if (json) process.stderr.write(`${JSON.stringify(value)}\n`);
    else {
      const at = value.span ? `${value.span.path}:${value.span.start.line}:${value.span.start.column}` : value.path;
      process.stderr.write(`${value.severity} ${value.code}${at ? ` (${at})` : ""}: ${value.message}\n`);
      if (value.details) process.stderr.write(`  ${value.details}\n`);
    }
  }
}

export async function importCommand(options: ImportCommandOptions): Promise<number> {
  const json = options.flags.has("json");
  const [format, source, project = "."] = options.positional;
  const importer = format === undefined ? undefined : importers[format];
  if (importer === undefined || source === undefined || options.positional.length > 3) {
    print(json, [diagnostic("PXCLI1901", `import requires a format (${importFormats.join(", ")}), ` +
      "a source directory and optionally a project directory", format)]);
    return 2;
  }
  try {
    const log = (message: string) => { if (!json) process.stderr.write(`${message}\n`); };
    const result = await importer.run(source, project, options, log);
    process.stdout.write(json ? `${JSON.stringify({imported: format, ...result.summary})}\n`
      : `${result.lines.join("\n")}\n`);
    return 0;
  } catch (error) {
    if (error instanceof ImportFailure)
      print(json, [diagnostic("PXCLI1902", error.message, project), ...error.diagnostics.slice(0, 50)]);
    else if (error instanceof ArtemisConfigError)
      print(json, [diagnostic("PXCLI1903", "Invalid import configuration", undefined, error.message)]);
    else if (error instanceof ArtemisSyntaxError)
      print(json, [diagnostic("PXCLI1904", "Artemis source could not be read", source, error.message)]);
    else print(json, [diagnostic("PXCLI1902", "Import failed", project,
      error instanceof Error ? error.stack ?? error.message : String(error))]);
    return 1;
  }
}
