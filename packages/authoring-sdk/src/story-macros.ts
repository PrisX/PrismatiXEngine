import {createHash} from "node:crypto";

import {fnv1a64 as stableId} from "./fnv1a64.js";
import {isSafeResourcePath} from "./resources.js";
import type {AuthoringDiagnostic, StoryArgument, StoryDocument, StoryNode} from "./types.js";

/**
 * Story macros: `[macro name param other=default]` ... `[endmacro]` defines a
 * block of Story lines, and `[name value other=value]` expands it where it
 * is called, replacing `%param` (and `%%` with `%`). Expansion happens before
 * compilation, so every expanded operation is an ordinary operation whose
 * diagnostics and source map point at the call. Operation ids derive from
 * the call's identity and the line's position in the body, so saves stay
 * valid while the body is unchanged.
 *
 * A project may also list macro files (`storyMacros` in prismatix.json)
 * whose definitions every Story can call. Such a file holds only macro
 * definitions and comments, so it emits no operation of its own.
 */

const macroNamePattern = /^[A-Za-z][A-Za-z0-9_-]*$/u;
const parameterPattern = /^[A-Za-z_][A-Za-z0-9_]*$/u;
const maximumDepth = 16;
const maximumMacroFiles = 64;

interface MacroParameter {
  readonly name: string;
  readonly defaultValue?: string;
}

export interface StoryMacroDefinition {
  readonly name: string;
  readonly parameters: readonly MacroParameter[];
  readonly body: string;
  readonly node: StoryNode;
  /** Project macro file that defines a shared macro; absent for a local one. */
  readonly origin?: string;
}

/** Shared macro definitions built once per build from `storyMacros` files. */
export interface StoryMacroLibrary {
  /** Changes whenever any macro file's path or text changes. */
  readonly fingerprint: string;
  readonly macros: ReadonlyMap<string, StoryMacroDefinition>;
}

export interface MacroExpansionOptions {
  readonly documentId: string;
  /** Command names a macro may not take: built-ins and extension commands. */
  readonly reserved: ReadonlySet<string>;
  readonly parse: (text: string, path: string) => StoryDocument;
  /** Project-wide macros; a local definition may not reuse their names. */
  readonly shared?: StoryMacroLibrary;
}

function issue(code: string, message: string, node: StoryNode, details?: string): AuthoringDiagnostic {
  return {severity: "error", code, message, path: node.span.path, span: node.span,
    ...(details === undefined ? {} : {details})};
}

function text(argument: StoryArgument): string | undefined {
  const {value} = argument;
  return typeof value === "string" ? value
    : typeof value === "number" || typeof value === "boolean" ? String(value) : undefined;
}

function where(definition: StoryMacroDefinition): string {
  return definition.origin === undefined ? `line ${definition.node.span.start.line}`
    : `${definition.origin}:${definition.node.span.start.line}`;
}

/** Replaces `%name` with the bound value and `%%` with `%`; other `%` stay. */
export function substituteMacroParameters(body: string, values: ReadonlyMap<string, string>): string {
  return body.replace(/%%|%([A-Za-z_][A-Za-z0-9_]*)/gu, (match, name: string | undefined) =>
    name === undefined ? "%" : values.get(name) ?? match);
}

/** Collects the `[macro]` definitions of one parsed Story or macro file. */
function collectDefinitions(document: StoryDocument, options: {
  readonly reserved: ReadonlySet<string>;
  readonly parse: (text: string, path: string) => StoryDocument;
  readonly shared?: StoryMacroLibrary;
  readonly origin?: string;
}, diagnostics: AuthoringDiagnostic[],
into = new Map<string, StoryMacroDefinition>()): Map<string, StoryMacroDefinition> {
  for (const node of document.nodes) {
    if (node.kind !== "command" || node.name !== "macro") continue;
    const nameArgument = node.arguments?.find((argument) => argument.name === undefined);
    const name = nameArgument === undefined ? undefined : text(nameArgument);
    if (name === undefined || !macroNamePattern.test(name)) {
      diagnostics.push(issue("PXSTORY1191", "macro needs a name: letters, digits, _ and -", node));
      continue;
    }
    const existing = into.get(name) ?? options.shared?.macros.get(name);
    if (options.reserved.has(name) || existing !== undefined) {
      diagnostics.push(issue("PXSTORY1191", existing === undefined
        ? `macro ${name} would hide the ${name} command`
        : existing.origin === undefined ? `macro ${name} is defined twice`
          : `macro ${name} is already defined at ${where(existing)}`, node));
      continue;
    }
    const parameters: MacroParameter[] = [];
    let valid = true;
    for (const argument of node.arguments ?? []) {
      if (argument === nameArgument) continue;
      const parameter = argument.name ?? text(argument);
      if (parameter === undefined || !parameterPattern.test(parameter) ||
          parameters.some((existingParameter) => existingParameter.name === parameter)) {
        diagnostics.push(issue("PXSTORY1192",
          `macro ${name} parameter ${argument.raw} must be a unique identifier`, node));
        valid = false;
        continue;
      }
      const defaultValue = argument.name === undefined ? undefined : text(argument);
      parameters.push({name: parameter, ...(defaultValue === undefined ? {} : {defaultValue})});
    }
    const body = node.text ?? "";
    const parsed = options.parse(body, document.path);
    if (parsed.nodes.some((inner) => inner.kind === "label" ||
        (inner.kind === "command" && (inner.name === "id" || inner.name === "macro")))) {
      diagnostics.push(issue("PXSTORY1193",
        `macro ${name} cannot contain labels, [id] or another [macro]; every call would repeat them`, node));
      valid = false;
    }
    if (valid) into.set(name, {name, parameters, body, node,
      ...(options.origin === undefined ? {} : {origin: options.origin})});
  }
  return into;
}

/** Returns a reason when an author-supplied `storyMacros` value is malformed. */
export function storyMacroFilesError(value: unknown): string | undefined {
  if (!Array.isArray(value) || value.length === 0 || value.length > maximumMacroFiles ||
      !value.every((path: unknown) => typeof path === "string" && path.endsWith(".pxstory") &&
        isSafeResourcePath(path) && !path.includes(":") &&
        path.split("/").every((part) => part.length > 0 && part !== ".")) ||
      new Set(value).size !== value.length)
    return `storyMacros must list 1-${maximumMacroFiles} distinct project-relative .pxstory files`;
  return undefined;
}

/**
 * Builds the project's shared macros from `storyMacros` files. Each file may
 * hold only `[macro]` definitions, comments and blank lines; every other
 * line is `PXSTORY1196`, because nothing would ever run it. Names must be
 * unique across all files.
 */
export function buildStoryMacroLibrary(files: readonly {readonly path: string; readonly source: string}[],
  options: {
    readonly reserved: ReadonlySet<string>;
    readonly parse: (text: string, path: string) => StoryDocument;
  }): {readonly library: StoryMacroLibrary; readonly diagnostics: readonly AuthoringDiagnostic[]} {
  const diagnostics: AuthoringDiagnostic[] = [];
  const macros = new Map<string, StoryMacroDefinition>();
  const fingerprint = createHash("sha256");
  for (const file of files) {
    fingerprint.update(`${file.path}\u0000${file.source}\u0000`);
    const document = options.parse(file.source, file.path);
    diagnostics.push(...document.diagnostics);
    for (const node of document.nodes) {
      if (node.kind === "comment" || (node.kind === "command" && node.name === "macro")) continue;
      diagnostics.push(issue("PXSTORY1196",
        "A storyMacros file holds only [macro] definitions and comments", node));
    }
    collectDefinitions(document, {...options, origin: file.path}, diagnostics, macros);
  }
  return {library: {fingerprint: fingerprint.digest("hex"), macros}, diagnostics};
}

export function expandStoryMacros(document: StoryDocument, options: MacroExpansionOptions): {
  readonly nodes: readonly StoryNode[];
  readonly diagnostics: readonly AuthoringDiagnostic[];
} {
  const diagnostics: AuthoringDiagnostic[] = [];
  const macros = collectDefinitions(document, options, diagnostics);
  const find = (name: string): StoryMacroDefinition | undefined =>
    macros.get(name) ?? options.shared?.macros.get(name);

  const expand = (nodes: readonly StoryNode[], depth: number, stack: readonly string[]): StoryNode[] => {
    const output: StoryNode[] = [];
    for (const node of nodes) {
      if (node.kind === "command" && node.name === "macro") continue;
      if (node.kind === "command" && node.name === "endmacro") {
        diagnostics.push(issue("PXSTORY1190", "[endmacro] closes no [macro]", node));
        continue;
      }
      const macro = node.kind === "command" && node.name !== undefined &&
        !options.reserved.has(node.name) ? find(node.name) : undefined;
      if (macro === undefined) {
        output.push(node);
        continue;
      }
      if (stack.includes(macro.name) || depth >= maximumDepth) {
        diagnostics.push(issue("PXSTORY1194", `macro ${macro.name} calls itself`, node,
          [...stack, macro.name].join(" > ")));
        continue;
      }
      const values = new Map<string, string>();
      let bound = true;
      let position = 0;
      for (const argument of node.arguments ?? []) {
        const value = text(argument);
        const parameter = argument.name === undefined ? macro.parameters[position++]
          : macro.parameters.find((candidate) => candidate.name === argument.name);
        if (parameter === undefined || value === undefined || values.has(parameter.name)) {
          diagnostics.push(issue("PXSTORY1195", argument.name === undefined
            ? `macro ${macro.name} takes ${macro.parameters.length} value(s)`
            : `macro ${macro.name} has no single parameter ${argument.name}`, node,
          macro.origin === undefined ? undefined : `defined at ${where(macro)}`));
          bound = false;
          continue;
        }
        values.set(parameter.name, value);
      }
      for (const parameter of macro.parameters) {
        if (values.has(parameter.name)) continue;
        if (parameter.defaultValue !== undefined) values.set(parameter.name, parameter.defaultValue);
        else {
          diagnostics.push(issue("PXSTORY1195", `macro ${macro.name} needs ${parameter.name}`, node,
            macro.origin === undefined ? undefined : `defined at ${where(macro)}`));
          bound = false;
        }
      }
      if (!bound) continue;
      // An authored [id] right before the call pins the whole expansion.
      let base = node.externalSourceId ?? node.id;
      const previous = output.at(-1);
      if (previous?.kind === "command" && previous.name === "id") {
        const pinned = previous.arguments?.[0] === undefined ? undefined : text(previous.arguments[0]);
        if (pinned !== undefined) {
          output.pop();
          base = `story-${stableId(`${options.documentId}:${pinned}`)}`;
        }
      }
      const parsed = options.parse(substituteMacroParameters(macro.body, values),
        macro.origin ?? document.path);
      const context = (line: number | undefined) => `in macro ${macro.name}${
        macro.origin === undefined ? "" : ` (${where(macro)})`}${
        line === undefined ? "" : `, body line ${line}`}`;
      for (const diagnostic of parsed.diagnostics)
        diagnostics.push({...diagnostic, path: node.span.path, span: node.span,
          details: [context(diagnostic.span?.start.line), diagnostic.details]
            .filter((part) => part !== undefined).join(": ")});
      // Expanded lines, and each of their arguments, report at the call;
      // compileStory names the definition in the diagnostic details.
      const origin = `${macro.name} (${where(macro)})${
        node.macroOrigin === undefined ? "" : ` via ${node.macroOrigin}`}`;
      const atCall = (values: readonly StoryArgument[] | undefined) =>
        values?.map((argument) => ({...argument, span: node.span}));
      const placed = parsed.nodes.map((inner, index): StoryNode => {
        const id = `${base}/m${index}`;
        const args = atCall(inner.arguments);
        const options = atCall(inner.options);
        return {...inner, id, externalSourceId: id, span: node.span, macroOrigin: origin,
          ...(args === undefined ? {} : {arguments: args}),
          ...(options === undefined ? {} : {options})};
      });
      output.push(...expand(placed, depth + 1, [...stack, macro.name]));
    }
    return output;
  };
  return {nodes: expand(document.nodes, 0, []), diagnostics};
}
