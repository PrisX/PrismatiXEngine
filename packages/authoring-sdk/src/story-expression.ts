import type {GameVariable, JsonPrimitive, JsonValue} from "./types.js";

type ExpressionType = GameVariable["type"] | "unknown";

interface ExpressionNodeBase {
  readonly valueType: ExpressionType;
}

interface LiteralNode extends ExpressionNodeBase {
  readonly kind: "literal";
  readonly value: JsonPrimitive;
}

interface VariableNode extends ExpressionNodeBase {
  readonly kind: "variable";
  readonly name: string;
  readonly unresolved: boolean;
}

interface UnaryNode extends ExpressionNodeBase {
  readonly kind: "unary";
  readonly op: "not" | "negate";
  readonly operand: ExpressionNode;
}

interface BinaryNode extends ExpressionNodeBase {
  readonly kind: "binary";
  readonly op: "add" | "subtract" | "multiply" | "divide" | "modulo" |
    "equal" | "not-equal" | "less" | "less-equal" | "greater" |
    "greater-equal" | "and" | "or";
  readonly left: ExpressionNode;
  readonly right: ExpressionNode;
}

type ExpressionNode = LiteralNode | VariableNode | UnaryNode | BinaryNode;

interface Token {
  readonly kind: "identifier" | "literal" | "operator" | "left" | "right" | "end";
  readonly text: string;
  readonly value?: JsonPrimitive;
  readonly prefixed?: boolean;
  readonly offset: number;
}

export interface StoryExpressionResult {
  readonly encoded?: string;
  readonly error?: string;
}

const binaryOperators: Readonly<Record<string, {readonly precedence: number; readonly name: BinaryNode["op"]}>> = {
  "||": {precedence: 1, name: "or"},
  or: {precedence: 1, name: "or"},
  "&&": {precedence: 2, name: "and"},
  and: {precedence: 2, name: "and"},
  "==": {precedence: 3, name: "equal"},
  "!=": {precedence: 3, name: "not-equal"},
  "<": {precedence: 4, name: "less"},
  "<=": {precedence: 4, name: "less-equal"},
  ">": {precedence: 4, name: "greater"},
  ">=": {precedence: 4, name: "greater-equal"},
  "+": {precedence: 5, name: "add"},
  "-": {precedence: 5, name: "subtract"},
  "*": {precedence: 6, name: "multiply"},
  "/": {precedence: 6, name: "divide"},
  "%": {precedence: 6, name: "modulo"},
};

function tokenize(source: string): Token[] {
  const tokens: Token[] = [];
  let offset = 0;
  while (offset < source.length) {
    if (/\s/u.test(source[offset]!)) {
      offset += 1;
      continue;
    }
    const start = offset;
    const character = source[offset]!;
    if (character === "(") {
      tokens.push({kind: "left", text: character, offset});
      offset += 1;
      continue;
    }
    if (character === ")") {
      tokens.push({kind: "right", text: character, offset});
      offset += 1;
      continue;
    }
    const pair = source.slice(offset, offset + 2);
    if (["&&", "||", "==", "!=", "<=", ">="].includes(pair)) {
      tokens.push({kind: "operator", text: pair, offset});
      offset += 2;
      continue;
    }
    if (["!", "-", "+", "*", "/", "%", "<", ">"].includes(character)) {
      tokens.push({kind: "operator", text: character, offset});
      offset += 1;
      continue;
    }
    if (character === "\"" || character === "'") {
      const quote = character;
      offset += 1;
      let value = "";
      let closed = false;
      while (offset < source.length) {
        const current = source[offset]!;
        if (current === quote) {
          offset += 1;
          closed = true;
          break;
        }
        if (current === "\\") {
          offset += 1;
          if (offset >= source.length) break;
          const escaped = source[offset]!;
          const escapes: Readonly<Record<string, string>> = {
            n: "\n", r: "\r", t: "\t", "\\": "\\", "\"": "\"", "'": "'",
          };
          value += escapes[escaped] ?? escaped;
          offset += 1;
          continue;
        }
        value += current;
        offset += 1;
      }
      if (!closed) throw new Error(`Unterminated string literal at column ${start + 1}`);
      tokens.push({kind: "literal", text: source.slice(start, offset), value, offset: start});
      continue;
    }
    const number = source.slice(offset).match(/^(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/u)?.[0];
    if (number !== undefined) {
      const value = Number(number);
      if (!Number.isFinite(value)) throw new Error(`Number is not finite at column ${start + 1}`);
      tokens.push({kind: "literal", text: number, value, offset});
      offset += number.length;
      continue;
    }
    const identifier = source.slice(offset).match(/^\$?[A-Za-z_][A-Za-z0-9._-]*/u)?.[0];
    if (identifier !== undefined) {
      const normalized = identifier.startsWith("$") ? identifier.slice(1) : identifier;
      if (normalized === "true" || normalized === "false") {
        tokens.push({kind: "literal", text: identifier, value: normalized === "true", offset});
      } else if (normalized === "null") {
        tokens.push({kind: "literal", text: identifier, value: null, offset});
      } else if (["and", "or", "not"].includes(normalized)) {
        tokens.push({kind: "operator", text: normalized, offset});
      } else {
        tokens.push({kind: "identifier", text: normalized, prefixed: identifier.startsWith("$"), offset});
      }
      offset += identifier.length;
      continue;
    }
    throw new Error(`Unexpected '${character}' at column ${start + 1}`);
  }
  tokens.push({kind: "end", text: "", offset: source.length});
  return tokens;
}

class Parser {
  private index = 0;

  public constructor(
    private readonly tokens: readonly Token[],
    private readonly variables: ReadonlyMap<string, GameVariable> | undefined,
  ) {}

  public parse(): ExpressionNode {
    const expression = this.parseBinary(1);
    const trailing = this.current();
    if (trailing.kind !== "end") {
      throw new Error(`Unexpected '${trailing.text}' at column ${trailing.offset + 1}`);
    }
    return this.resolveLegacyStringLiterals(expression);
  }

  private current(): Token {
    return this.tokens[this.index] ?? {kind: "end", text: "", offset: 0};
  }

  private consume(): Token {
    const token = this.current();
    this.index += 1;
    return token;
  }

  private parseBinary(minimumPrecedence: number): ExpressionNode {
    let left = this.parseUnary();
    while (true) {
      const token = this.current();
      const operator = token.kind === "operator" ? binaryOperators[token.text] : undefined;
      if (operator === undefined || operator.precedence < minimumPrecedence) break;
      this.consume();
      const right = this.parseBinary(operator.precedence + 1);
      left = this.binary(operator.name, left, right, token);
    }
    return left;
  }

  private parseUnary(): ExpressionNode {
    const token = this.current();
    if (token.kind === "operator" && ["!", "not", "-"].includes(token.text)) {
      this.consume();
      const operand = this.parseUnary();
      const op: UnaryNode["op"] = token.text === "-" ? "negate" : "not";
      if (op === "not" && operand.valueType !== "boolean" && operand.valueType !== "unknown") {
        throw new Error(`Operator '${token.text}' requires a boolean operand`);
      }
      if (op === "negate" && !this.numeric(operand.valueType) && operand.valueType !== "unknown") {
        throw new Error("Unary '-' requires a numeric operand");
      }
      return {kind: "unary", op, operand, valueType: op === "not" ? "boolean" : operand.valueType};
    }
    return this.parsePrimary();
  }

  private parsePrimary(): ExpressionNode {
    const token = this.consume();
    if (token.kind === "literal") {
      const value = token.value ?? null;
      const valueType: ExpressionType = value === null ? "unknown" :
        typeof value === "number" ? (Number.isInteger(value) ? "integer" : "number") :
        typeof value === "boolean" ? "boolean" : "string";
      return {kind: "literal", value, valueType};
    }
    if (token.kind === "identifier") {
      const descriptor = this.variables?.get(token.text);
      return {
        kind: "variable",
        name: token.text,
        valueType: descriptor?.type ?? "unknown",
        unresolved: this.variables !== undefined && descriptor === undefined && token.prefixed !== true,
      };
    }
    if (token.kind === "left") {
      const expression = this.parseBinary(1);
      const right = this.consume();
      if (right.kind !== "right") throw new Error(`Expected ')' at column ${right.offset + 1}`);
      return expression;
    }
    throw new Error(`Expected an expression at column ${token.offset + 1}`);
  }

  private binary(op: BinaryNode["op"], left: ExpressionNode, right: ExpressionNode, token: Token): BinaryNode {
    if (op === "and" || op === "or") {
      this.require(left, "boolean", token);
      this.require(right, "boolean", token);
      return {kind: "binary", op, left, right, valueType: "boolean"};
    }
    if (["equal", "not-equal"].includes(op)) {
      if (left.valueType !== "unknown" && right.valueType !== "unknown" &&
          left.valueType !== right.valueType && !(this.numeric(left.valueType) && this.numeric(right.valueType))) {
        throw new Error(`Operator '${token.text}' compares incompatible values`);
      }
      return {kind: "binary", op, left, right, valueType: "boolean"};
    }
    if (["less", "less-equal", "greater", "greater-equal"].includes(op)) {
      const comparable = (this.numeric(left.valueType) && this.numeric(right.valueType)) ||
        (left.valueType === "string" && right.valueType === "string") ||
        left.valueType === "unknown" || right.valueType === "unknown";
      if (!comparable) throw new Error(`Operator '${token.text}' requires numeric or string operands`);
      return {kind: "binary", op, left, right, valueType: "boolean"};
    }
    if (op === "add" && left.valueType === "string" && right.valueType === "string") {
      return {kind: "binary", op, left, right, valueType: "string"};
    }
    if ((!this.numeric(left.valueType) && left.valueType !== "unknown") ||
        (!this.numeric(right.valueType) && right.valueType !== "unknown")) {
      throw new Error(`Operator '${token.text}' requires numeric operands`);
    }
    if (op === "modulo" && ((left.valueType !== "integer" && left.valueType !== "unknown") ||
        (right.valueType !== "integer" && right.valueType !== "unknown"))) {
      throw new Error("Operator '%' requires integer operands");
    }
    return {kind: "binary", op, left, right,
      valueType: op === "divide" || left.valueType === "number" || right.valueType === "number" ? "number" : "integer"};
  }

  private require(node: ExpressionNode, type: ExpressionType, token: Token): void {
    if (node.valueType !== type && node.valueType !== "unknown") {
      throw new Error(`Operator '${token.text}' requires ${type} operands`);
    }
  }

  private numeric(type: ExpressionType): boolean {
    return type === "integer" || type === "number";
  }

  private resolveLegacyStringLiterals(node: ExpressionNode): ExpressionNode {
    if (node.kind === "unary") {
      return {...node, operand: this.resolveLegacyStringLiterals(node.operand)};
    }
    if (node.kind !== "binary") {
      if (node.kind === "variable" && node.unresolved) {
        throw new Error(`Unknown game variable '${node.name}'`);
      }
      return node;
    }
    let left = this.resolveChild(node.left, node.right, node.op);
    let right = this.resolveChild(node.right, left, node.op);
    left = this.resolveLegacyStringLiterals(left);
    right = this.resolveLegacyStringLiterals(right);
    return {...node, left, right};
  }

  private resolveChild(candidate: ExpressionNode, peer: ExpressionNode, op: BinaryNode["op"]): ExpressionNode {
    if (candidate.kind !== "variable" || !candidate.unresolved ||
        !["equal", "not-equal", "less", "less-equal", "greater", "greater-equal"].includes(op) ||
        peer.valueType !== "string") return candidate;
    return {kind: "literal", value: candidate.name, valueType: "string"};
  }
}

function runtimeExpression(node: ExpressionNode): JsonValue {
  switch (node.kind) {
    case "literal": return {kind: "literal", value: node.value};
    case "variable": return {kind: "variable", name: node.name};
    case "unary": return {kind: "unary", op: node.op, operand: runtimeExpression(node.operand)};
    case "binary": return {
      kind: "binary", op: node.op,
      left: runtimeExpression(node.left), right: runtimeExpression(node.right),
    };
  }
}

export function parseStoryExpression(
  source: string,
  variables?: readonly GameVariable[],
): StoryExpressionResult {
  if (source.trim() === "") return {error: "Condition expression is empty"};
  try {
    const index = variables === undefined ? undefined : new Map(variables.map((value) => [value.name, value]));
    const parsed = new Parser(tokenize(source), index).parse();
    if (parsed.valueType !== "boolean" && parsed.valueType !== "unknown") {
      return {error: "Condition expression must produce a boolean value"};
    }
    return {encoded: JSON.stringify(runtimeExpression(parsed))};
  } catch (caught) {
    return {error: caught instanceof Error ? caught.message : "Invalid condition expression"};
  }
}
