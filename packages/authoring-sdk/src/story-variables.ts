import type {AuthoringDiagnostic, GameDocument, GameVariable, JsonPrimitive, SourceSpan,
  StoryNode} from "./types.js";

/**
 * Story variable declarations: `[var affection=0]`, `[var met_aoi=false
 * scope=profile]`, `[var name="湊" type=string]`. A declaration emits no
 * operation; the build merges every declaration into the compiled game
 * catalog, exactly as if it had been written in the `.pxgame` file.
 */

const variableNamePattern = /^[A-Za-z0-9][A-Za-z0-9._-]*$/u;
const variableTypes = ["boolean", "integer", "number", "string"] as const;
const variableScopes = ["session", "profile"] as const;

export interface StoryVariableDeclaration extends GameVariable {
  readonly span: SourceSpan;
}

function issue(code: string, message: string, span: SourceSpan, details?: string): AuthoringDiagnostic {
  return {severity: "error", code, message, path: span.path, span,
    ...(details === undefined ? {} : {details})};
}

function inferredType(value: JsonPrimitive): GameVariable["type"] {
  return typeof value === "boolean" ? "boolean"
    : typeof value === "number" ? Number.isInteger(value) ? "integer" : "number" : "string";
}

/** Reads one `[var name=default scope=... type=...]` node. */
export function storyVariableDeclaration(node: StoryNode,
  diagnostics: AuthoringDiagnostic[]): StoryVariableDeclaration | undefined {
  const invalid = (message: string, span = node.span): undefined => {
    diagnostics.push(issue("PXSTORY1250", message, span));
    return undefined;
  };
  let scope: string | undefined;
  let type: string | undefined;
  let variable: {name: string; raw: string; value: unknown; span: SourceSpan} | undefined;
  for (const argument of node.arguments ?? []) {
    if (argument.name === "scope") scope = String(argument.value);
    else if (argument.name === "type") type = String(argument.value);
    else if (argument.name === undefined)
      return invalid("var takes name=default, for example [var affection=0]", argument.span);
    else if (variable !== undefined)
      return invalid("Declare one variable per [var]", argument.span);
    else variable = {name: argument.name, raw: argument.raw, value: argument.value, span: argument.span};
  }
  if (variable === undefined) return invalid("var needs name=default, for example [var affection=0]");
  if (!variableNamePattern.test(variable.name))
    return invalid(`Variable name must be an identifier: ${variable.name}`);
  if (scope !== undefined && !(variableScopes as readonly string[]).includes(scope))
    return invalid("var scope must be session or profile");
  if (type !== undefined && !(variableTypes as readonly string[]).includes(type))
    return invalid("var type must be boolean, integer, number or string");
  if (variable.value === null || typeof variable.value === "object")
    return invalid("var default must be a boolean, number or string", variable.span);
  let value = variable.value as JsonPrimitive;
  // An explicit string type keeps an unquoted default as written.
  if (type === "string" && typeof value !== "string") value = variable.raw;
  const resolvedType = (type ?? inferredType(value)) as GameVariable["type"];
  const matches = resolvedType === "integer" ? typeof value === "number" && Number.isSafeInteger(value)
    : resolvedType === "number" ? typeof value === "number" && Number.isFinite(value)
      : typeof value === resolvedType;
  if (!matches) return invalid(`Default for ${variable.name} does not match ${resolvedType}`, variable.span);
  return {name: variable.name, type: resolvedType, default: value,
    scope: (scope ?? "session") as GameVariable["scope"], span: node.span};
}

function same(left: GameVariable, right: GameVariable): boolean {
  return left.type === right.type && left.scope === right.scope && left.default === right.default;
}

function describe(variable: GameVariable): string {
  return `${variable.type} ${variable.scope} default ${JSON.stringify(variable.default)}`;
}

/**
 * Adds Story declarations to a game catalog. Repeating an identical
 * declaration is allowed; any disagreement with another declaration or with
 * the catalog is `PXSTORY1251` at the later declaration.
 */
export function mergeStoryVariables(game: GameDocument, declarations: readonly StoryVariableDeclaration[],
  gameCatalogPath = "Content/game.pxgame"): {readonly game: GameDocument;
    readonly diagnostics: readonly AuthoringDiagnostic[]} {
  const diagnostics: AuthoringDiagnostic[] = [];
  const catalog = new Map(game.variables.map((variable) => [variable.name, variable]));
  const declared = new Map<string, StoryVariableDeclaration>();
  const added: GameVariable[] = [];
  for (const declaration of declarations) {
    const existing = catalog.get(declaration.name);
    const earlier = declared.get(declaration.name);
    if (existing !== undefined && !same(existing, declaration)) {
      diagnostics.push(issue("PXSTORY1251",
        `Variable ${declaration.name} is declared differently in ${gameCatalogPath}`, declaration.span,
        `${gameCatalogPath}: ${describe(existing)}; here: ${describe(declaration)}`));
      continue;
    }
    if (earlier !== undefined && !same(earlier, declaration)) {
      diagnostics.push(issue("PXSTORY1251",
        `Variable ${declaration.name} is already declared differently at ${earlier.span.path}:${
          earlier.span.start.line}`, declaration.span,
        `first: ${describe(earlier)}; here: ${describe(declaration)}`));
      continue;
    }
    if (existing === undefined && earlier === undefined)
      added.push({name: declaration.name, type: declaration.type,
        default: declaration.default, scope: declaration.scope});
    declared.set(declaration.name, earlier ?? declaration);
  }
  return {game: added.length === 0 ? game : {...game, variables: [...game.variables, ...added]},
    diagnostics};
}
