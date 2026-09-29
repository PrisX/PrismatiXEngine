import {fnv1a64 as stableId} from "./fnv1a64.js";
import type {AuthoringDiagnostic, StoryArgument, StoryDocument, StoryNode} from "./types.js";

/**
 * Story macros: `[macro name param other=default]` ... `[endmacro]` defines a
 * block of Story lines, and `[name value other=value]` expands it where it
 * is called, replacing `%param` (and `%%` with `%`). Expansion happens before
 * compilation, so every expanded operation is an ordinary operation whose
 * diagnostics and source map point at the call. Operation ids derive from
 * the call's identity and the line's position in the body, so saves stay
 * valid while the body is unchanged.
 */

const macroNamePattern = /^[A-Za-z][A-Za-z0-9_-]*$/u;
const parameterPattern = /^[A-Za-z_][A-Za-z0-9_]*$/u;
const maximumDepth = 16;

interface MacroParameter {
  readonly name: string;
  readonly defaultValue?: string;
}

interface MacroDefinition {
  readonly name: string;
  readonly parameters: readonly MacroParameter[];
  readonly body: string;
  readonly node: StoryNode;
}

export interface MacroExpansionOptions {
  readonly documentId: string;
  /** Command names a macro may not take: built-ins and extension commands. */
  readonly reserved: ReadonlySet<string>;
  readonly parse: (text: string, path: string) => StoryDocument;
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

/** Replaces `%name` with the bound value and `%%` with `%`; other `%` stay. */
export function substituteMacroParameters(body: string, values: ReadonlyMap<string, string>): string {
  return body.replace(/%%|%([A-Za-z_][A-Za-z0-9_]*)/gu, (match, name: string | undefined) =>
    name === undefined ? "%" : values.get(name) ?? match);
}

export function expandStoryMacros(document: StoryDocument, options: MacroExpansionOptions): {
  readonly nodes: readonly StoryNode[];
  readonly diagnostics: readonly AuthoringDiagnostic[];
} {
  const diagnostics: AuthoringDiagnostic[] = [];
  const macros = new Map<string, MacroDefinition>();
  for (const node of document.nodes) {
    if (node.kind !== "command" || node.name !== "macro") continue;
    const nameArgument = node.arguments?.find((argument) => argument.name === undefined);
    const name = nameArgument === undefined ? undefined : text(nameArgument);
    if (name === undefined || !macroNamePattern.test(name)) {
      diagnostics.push(issue("PXSTORY1191", "macro needs a name: letters, digits, _ and -", node));
      continue;
    }
    if (options.reserved.has(name) || macros.has(name)) {
      diagnostics.push(issue("PXSTORY1191", macros.has(name)
        ? `macro ${name} is defined twice` : `macro ${name} would hide the ${name} command`, node));
      continue;
    }
    const parameters: MacroParameter[] = [];
    let valid = true;
    for (const argument of node.arguments ?? []) {
      if (argument === nameArgument) continue;
      const parameter = argument.name ?? text(argument);
      if (parameter === undefined || !parameterPattern.test(parameter) ||
          parameters.some((existing) => existing.name === parameter)) {
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
    if (valid) macros.set(name, {name, parameters, body, node});
  }

  const expand = (nodes: readonly StoryNode[], depth: number, stack: readonly string[]): StoryNode[] => {
    const output: StoryNode[] = [];
    for (const node of nodes) {
      if (node.kind === "command" && node.name === "macro") continue;
      if (node.kind === "command" && node.name === "endmacro") {
        diagnostics.push(issue("PXSTORY1190", "[endmacro] closes no [macro]", node));
        continue;
      }
      const macro = node.kind === "command" && node.name !== undefined
        ? macros.get(node.name) : undefined;
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
            : `macro ${macro.name} has no single parameter ${argument.name}`, node));
          bound = false;
          continue;
        }
        values.set(parameter.name, value);
      }
      for (const parameter of macro.parameters) {
        if (values.has(parameter.name)) continue;
        if (parameter.defaultValue !== undefined) values.set(parameter.name, parameter.defaultValue);
        else {
          diagnostics.push(issue("PXSTORY1195", `macro ${macro.name} needs ${parameter.name}`, node));
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
      const parsed = options.parse(substituteMacroParameters(macro.body, values), document.path);
      const where = (line: number | undefined) =>
        `in macro ${macro.name}${line === undefined ? "" : `, body line ${line}`}`;
      for (const diagnostic of parsed.diagnostics)
        diagnostics.push({...diagnostic, path: node.span.path, span: node.span,
          details: [where(diagnostic.span?.start.line), diagnostic.details]
            .filter((part) => part !== undefined).join(": ")});
      const placed = parsed.nodes.map((inner, index): StoryNode => {
        const id = `${base}/m${index}`;
        return {...inner, id, externalSourceId: id, span: node.span};
      });
      output.push(...expand(placed, depth + 1, [...stack, macro.name]));
    }
    return output;
  };
  return {nodes: expand(document.nodes, 0, []), diagnostics};
}
