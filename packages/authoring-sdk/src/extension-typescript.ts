import type {ExtensionAction, ExtensionCommand, ExtensionManifest, ExtensionParameter} from "./types.js";

/**
 * TypeScript extensions. An extension whose entry (or a listed module) is a
 * `.ts` file is bundled by the PrismatiX CLI into one JavaScript module beside
 * it (`main.ts` -> `main.js`); the Player only ever loads JavaScript.
 */

/** The declaration file `prismatix extension types` writes beside a manifest. */
export const EXTENSION_TYPES_FILE = "prismatix-extension.d.ts";

/** A TypeScript source an extension may list (not a declaration file or TSX). */
export function isTypeScriptExtensionSource(path: string): boolean {
  return /\.ts$/iu.test(path) && !/\.d\.ts$/iu.test(path);
}

/** True when the CLI must bundle this extension before the Player can load it. */
export function extensionUsesTypeScript(manifest: Pick<ExtensionManifest, "entry" | "modules">): boolean {
  return [manifest.entry, ...(manifest.modules ?? [])]
    .some((path) => typeof path === "string" && isTypeScriptExtensionSource(path));
}

/** Where the bundled JavaScript of a TypeScript entry is published, relative to its manifest. */
export function extensionBundleEntry(entry: string): string {
  return entry.replace(/\.ts$/iu, ".js");
}

/**
 * The manifest the Player receives for a bundled extension: the JavaScript
 * bundle is the entry, every module is inside it, and the language is the
 * Player's (TypeScript is an authoring language).
 */
export function bundledExtensionManifest<T extends ExtensionManifest>(manifest: T, entry: string): T {
  const {modules: _modules, ...rest} = manifest;
  return {...rest, language: "javascript", entry} as T;
}

function comment(lines: readonly (string | undefined)[], indent: string): string {
  const text = lines.filter((line): line is string => line !== undefined && line.trim().length > 0)
    .map((line) => line.replaceAll("*/", "*\\/").trim());
  if (text.length === 0) return "";
  if (text.length === 1) return `${indent}/** ${text[0]} */\n`;
  return `${indent}/**\n${text.map((line) => `${indent} * ${line}`).join("\n")}\n${indent} */\n`;
}

function parameterType(parameter: ExtensionParameter): string {
  switch (parameter.type) {
    case "null": return "null";
    case "boolean": return "boolean";
    case "integer":
    case "number": return "number";
    case "string": {
      const choices = (parameter.enum ?? []).filter((choice): choice is string => typeof choice === "string");
      return choices.length > 0 && choices.length === (parameter.enum ?? []).length
        ? choices.map((choice) => JSON.stringify(choice)).join(" | ") : "string";
    }
    case "vec2": return "Vec2Value";
    case "rect": return "Rect";
    case "color": return "Color";
    case "uuid":
    case "token": return "string";
    case "resource": return "ResourceValue";
    case "array": return "readonly RuntimeValue[]";
    case "object": return "{readonly [key: string]: RuntimeValue}";
  }
  return "RuntimeValue";
}

function argumentsType(parameters: readonly ExtensionParameter[], additional: boolean, indent: string): string {
  const lines = parameters.map((parameter) => {
    const present = parameter.required === true ||
      (parameter.default !== undefined && (parameter.default !== null || parameter.type === "null"));
    const range = parameter.range === undefined ? undefined : [
      parameter.range.minimum === undefined ? undefined : `minimum ${parameter.range.minimum}`,
      parameter.range.maximum === undefined ? undefined : `maximum ${parameter.range.maximum}`,
      parameter.range.step === undefined ? undefined : `step ${parameter.range.step}`,
    ].filter(Boolean).join(", ");
    return comment([parameter.displayName, parameter.description,
      parameter.default === undefined ? undefined : `Default: ${JSON.stringify(parameter.default)}`,
      range === undefined || range.length === 0 ? undefined : `Range: ${range}`], `${indent}  `) +
      `${indent}  readonly ${JSON.stringify(parameter.name)}${present ? "" : "?"}: ${parameterType(parameter)};`;
  });
  if (additional) lines.push(`${indent}  readonly [extra: string]: unknown;`);
  return lines.length === 0 ? "{}" : `{\n${lines.join("\n")}\n${indent}}`;
}

function commandDeclaration(command: ExtensionCommand): string {
  const awaits = command.await === true;
  return comment([command.displayName, command.description,
    `${awaits ? "Story waits for it (await: true)" : "Story continues at once (await: false)"}; rollback: ${command.rollback ?? "boundary"}.`],
  "    ") + `    ${JSON.stringify(command.id)}: {\n` +
    `      readonly args: ${argumentsType(command.parameters, command.allowAdditionalParameters === true, "      ")};\n` +
    `      readonly await: ${awaits};\n` +
    `      readonly rollback: ${JSON.stringify(command.rollback ?? "boundary")};\n    };\n`;
}

function actionDeclaration(action: ExtensionAction): string {
  return comment([action.displayName, action.description, `reentry: ${action.reentry ?? "allow"}.`], "    ") +
    `    ${JSON.stringify(action.id)}: {\n` +
    `      readonly args: ${argumentsType(action.parameters, false, "      ")};\n` +
    `      readonly reentry: ${JSON.stringify(action.reentry ?? "allow")};\n    };\n`;
}

/**
 * Generates the declarations that type an extension's `defineCommand` and
 * `defineAction` calls from its manifest (`@prismatix/runtime` module
 * augmentation). The output is deterministic.
 */
export function extensionTypeDeclarations(manifest: ExtensionManifest, manifestName = "the extension manifest"): string {
  const capabilities = manifest.capabilities.length === 0 ? "never"
    : manifest.capabilities.map((capability) => JSON.stringify(capability)).join(" | ");
  return `// Generated by PrismatiX from ${manifestName.replaceAll("\n", " ")}. Do not edit:\n` +
    "// `prismatix extension types` rewrites it, and validate/build check against a fresh copy.\n" +
    "import type {Color, Rect, ResourceValue, RuntimeValue, Vec2Value} from \"@prismatix/runtime\";\n\n" +
    "declare module \"@prismatix/runtime\" {\n" +
    "  interface ExtensionManifestTypes {\n" +
    "    readonly declared: true;\n" +
    `    readonly id: ${JSON.stringify(manifest.id)};\n` +
    `    readonly capabilities: ${capabilities};\n  }\n\n` +
    "  interface ExtensionCommandDeclarations {\n" +
    manifest.commands.map(commandDeclaration).join("") + "  }\n\n" +
    "  interface ExtensionActionDeclarations {\n" +
    manifest.actions.map(actionDeclaration).join("") + "  }\n}\n";
}
