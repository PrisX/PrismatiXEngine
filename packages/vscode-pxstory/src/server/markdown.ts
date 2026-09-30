import {readFileSync} from "node:fs";
import {pathToFileURL} from "node:url";

import type {StoryCommandCompletion} from "@prismatix/authoring-sdk";

import type {MacroInfo} from "./analysis.js";
import type {CharacterInfo, ExpressionInfo, ProjectModel} from "./project.js";
import {commandSummaries} from "./story-roles.js";

const rasterImage = /\.(?:png|jpe?g|webp|bmp|gif)$/iu;

export function fileUri(fsPath: string): string {
  return pathToFileURL(fsPath).href;
}

function code(value: string): string {
  const fence = value.includes("`") ? "``" : "`";
  return `${fence}${value}${fence}`;
}

/** A small image preview for a project file: raster images directly, and a
 * layered .pxportrait through its base and expression layers. */
export function previewMarkdown(model: ProjectModel, path: string, width = 240): string | undefined {
  if (rasterImage.test(path)) {
    if (!model.fileExists(path)) return undefined;
    return `![${path}](${fileUri(model.toFsPath(path))}|width=${width})`;
  }
  if (/\.pxportrait$/iu.test(path)) {
    let layers: unknown;
    try {
      layers = (JSON.parse(readFileSync(model.toFsPath(path), "utf8")) as {layers?: unknown}).layers;
    } catch {
      return undefined;
    }
    if (!Array.isArray(layers)) return undefined;
    const images = layers
      .map((layer) => (layer as {path?: unknown}).path)
      .filter((layerPath): layerPath is string => typeof layerPath === "string" && rasterImage.test(layerPath))
      .slice(0, 2)
      .map((layerPath, index) => `![${layerPath}](${fileUri(model.toFsPath(layerPath))}|height=${
        index === 0 ? width : Math.round(width / 2)})`);
    return images.length === 0 ? undefined : images.join(" ");
  }
  return undefined;
}

export function commandSignature(command: StoryCommandCompletion): string {
  const parameters = command.parameters.map((parameter) => parameter.required
    ? `${parameter.name}=…` : `${parameter.name}?`);
  return `[${[command.id, ...parameters].join(" ")}]`;
}

export function commandMarkdown(command: StoryCommandCompletion, description?: string): string {
  const lines = [`**[${command.id}]**`];
  const summary = description ?? commandSummaries[command.id];
  if (summary !== undefined) lines.push("", summary);
  lines.push("", "```pxstory", commandSignature(command), "```");
  const valued = command.parameters.filter((parameter) => parameter.values !== undefined);
  for (const parameter of valued)
    lines.push(`- \`${parameter.name}\`: ${parameter.values!.map((value) => code(String(value))).join(", ")}`);
  return lines.join("\n");
}

export function macroMarkdown(macro: MacroInfo): string {
  const parameters = macro.parameters.map((parameter) => parameter.defaultValue === undefined
    ? parameter.name : `${parameter.name}=${parameter.defaultValue}`);
  const body = macro.body.split(/\r?\n/u);
  const shown = body.slice(0, 12).join("\n") + (body.length > 12 ? "\n; …" : "");
  return [`**macro** \`${macro.name}\``, "", "```pxstory",
    `[macro ${[macro.name, ...parameters].join(" ")}]`, shown, "[endmacro]", "```"].join("\n");
}

export function expressionMarkdown(model: ProjectModel, character: CharacterInfo,
  expression: ExpressionInfo): string {
  const lines = [`**${expression.name}** — ${character.displayName} (${code(character.aliases[0] ?? character.id)})`,
    "", `Aliases: ${expression.aliases.map(code).join(", ")}`];
  if (expression.assetPath !== undefined) {
    lines.push(`Image: ${code(expression.assetPath)}`);
    const preview = previewMarkdown(model, expression.assetPath);
    if (preview !== undefined) lines.push("", preview);
  }
  if (expression.facePath !== undefined) lines.push("", `Face: ${code(expression.facePath)}`);
  return lines.join("\n");
}

export function characterMarkdown(model: ProjectModel, character: CharacterInfo): string {
  const lines = [`**${character.displayName}** — character`, ""];
  lines.push(`Aliases: ${character.aliases.map(code).join(", ")}`);
  const localized = Object.entries(character.displayNames);
  if (localized.length > 0)
    lines.push(`Display names: ${localized.map(([locale, name]) => `${locale} ${name}`).join(", ")}`);
  const limit = 24;
  const listed = character.expressions.slice(0, limit)
    .map((expression) => `${code(expression.aliases[0] ?? expression.name)}`);
  lines.push(`Expressions (${character.expressions.length}): ${listed.join(", ")}${
    character.expressions.length > limit ? `, … ${character.expressions.length - limit} more` : ""}`);
  lines.push("", `[${character.source}](${fileUri(model.toFsPath(character.source))})`);
  return lines.join("\n");
}
