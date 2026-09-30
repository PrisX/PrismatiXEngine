import {existsSync} from "node:fs";
import {join} from "node:path";

/** How the extension starts the PrismatiX CLI for a project. */
export interface CliInvocation {
  readonly command: string;
  readonly args: readonly string[];
  /** Windows .cmd/.bat shims need a shell; every argument is then quoted. */
  readonly shell: boolean;
  readonly description: string;
}

export interface CliDiagnostic {
  readonly severity: "error" | "warning" | "information";
  readonly code: string;
  readonly message: string;
  readonly path?: string;
  readonly span?: {readonly path: string; readonly start: {readonly line: number; readonly column: number};
    readonly end: {readonly line: number; readonly column: number}};
  readonly hint?: string;
  readonly details?: string;
}

export type CliLine =
  | {readonly kind: "diagnostic"; readonly diagnostic: CliDiagnostic}
  | {readonly kind: "event"; readonly event: Readonly<Record<string, unknown>>; readonly text: string}
  | {readonly kind: "text"; readonly text: string};

export class CliNotFoundError extends Error {}

/**
 * The project's own pinned CLI (`node_modules/@prismatix/cli`) is used, as the
 * global `prismatix` launcher would delegate to it anyway. `configuredPath`
 * (the `prismatix.cli.path` setting) overrides it with a cli.js or an
 * executable.
 */
export function resolveCli(root: string, nodePath: string, configuredPath?: string): CliInvocation {
  const script = (path: string, description: string): CliInvocation =>
    ({command: nodePath, args: [path], shell: false, description});
  if (configuredPath !== undefined && configuredPath.trim().length > 0) {
    const path = configuredPath.trim();
    if (/\.[cm]?js$/iu.test(path)) return script(path, `${path} (prismatix.cli.path)`);
    return {command: path, args: [], shell: process.platform === "win32" && /\.(?:cmd|bat)$/iu.test(path),
      description: `${path} (prismatix.cli.path)`};
  }
  const local = join(root, "node_modules", "@prismatix", "cli", "dist", "src", "cli.js");
  if (existsSync(local)) return script(local, "the project's @prismatix/cli");
  throw new CliNotFoundError("This project has no local @prismatix/cli. Run `npm install` in the project " +
    "folder, or set prismatix.cli.path to a PrismatiX CLI.");
}

export function validateArguments(root: string, extra: readonly string[] = []): string[] {
  return ["validate", root, "--json", ...extra];
}

/** `prismatix dev <project> --at <scene>:<line>` with a one-based line. */
export function previewArguments(root: string, sceneId: string, line: number,
  extra: readonly string[] = []): string[] {
  return ["dev", root, "--at", `${sceneId}:${line}`, "--json", ...extra];
}

/** Quotes arguments for a Windows shell launch of a .cmd shim. */
export function shellArguments(args: readonly string[]): string[] {
  return args.map((argument) => /[\s"&|<>^]/u.test(argument)
    ? `"${argument.replace(/"/gu, "\"\"")}"` : argument);
}

/** One NDJSON line from `--json` output: a diagnostic, a progress event, or text. */
export function parseCliLine(line: string): CliLine {
  const trimmed = line.trim();
  if (trimmed.startsWith("{")) {
    try {
      const value = JSON.parse(trimmed) as Record<string, unknown>;
      if (typeof value.code === "string" && typeof value.message === "string" &&
          ["error", "warning"].includes(String(value.severity)) && value.event === undefined)
        return {kind: "diagnostic", diagnostic: value as unknown as CliDiagnostic};
      return {kind: "event", event: value, text: typeof value.message === "string" ? value.message : trimmed};
    } catch { /* plain text that starts with a brace */ }
  }
  return {kind: "text", text: line};
}

export function formatDiagnostic(diagnostic: CliDiagnostic): string {
  const at = diagnostic.span === undefined ? diagnostic.path
    : `${diagnostic.span.path}:${diagnostic.span.start.line}:${diagnostic.span.start.column}`;
  const detail = diagnostic.hint ?? diagnostic.details;
  return `${diagnostic.severity} ${diagnostic.code}${at === undefined ? "" : ` (${at})`}: ${diagnostic.message}` +
    (detail === undefined ? "" : `\n  ${detail}`);
}

/** Explains a failed `prismatix dev --at` run in one line for a notification. */
export function describePreviewFailure(exitCode: number | null, diagnostics: readonly CliDiagnostic[]): string {
  const unsupported = diagnostics.find((diagnostic) =>
    /--at\b/u.test(`${diagnostic.message} ${diagnostic.hint ?? ""} ${diagnostic.details ?? ""}`) &&
    /Unknown option|Invalid command arguments/iu.test(`${diagnostic.message} ${diagnostic.hint ?? ""} ${diagnostic.details ?? ""}`));
  if (unsupported !== undefined)
    return "This project's PrismatiX CLI does not support `prismatix dev --at` yet, so Preview From Here " +
      `cannot start at a line. Update @prismatix/cli in the project. CLI: ${
        (unsupported.hint ?? unsupported.details ?? unsupported.message).replace(/^Error:\s*/u, "")}`;
  const first = diagnostics.find((diagnostic) => diagnostic.severity === "error") ?? diagnostics[0];
  if (first !== undefined) {
    const detail = first.hint ?? first.details;
    return `Preview failed: ${first.code} ${first.message}${detail === undefined ? "" : ` — ${detail.split("\n")[0]}`}`;
  }
  return `Preview exited with code ${exitCode ?? "unknown"}; see the PrismatiX output for details.`;
}
