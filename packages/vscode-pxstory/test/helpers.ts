import {readFileSync} from "node:fs";
import {join} from "node:path";
import {fileURLToPath, pathToFileURL} from "node:url";

import {TextDocument} from "vscode-languageserver-textdocument";

/** Package root: tests run from out/test/*.js. */
export const packageRoot = fileURLToPath(new URL("../../", import.meta.url));
export const fixtures = join(packageRoot, "test", "fixtures");
export const scenesRoot = join(fixtures, "scenes");
export const minimalRoot = join(fixtures, "minimal");

let version = 0;

/** An in-memory document for a fixture file, optionally with replaced text. */
export function fixtureDocument(root: string, path: string, text?: string): TextDocument {
  const fsPath = join(root, ...path.split("/"));
  return TextDocument.create(pathToFileURL(fsPath).href, "pxstory", ++version,
    text ?? readFileSync(fsPath, "utf8"));
}

/** The position just after `needle` (or `offset` characters into it) on the
 * first line that contains it. */
export function positionOf(document: TextDocument, needle: string, offset = needle.length):
  {line: number; character: number} {
  const lines = document.getText().split(/\r?\n/u);
  const line = lines.findIndex((text) => text.includes(needle));
  if (line < 0) throw new Error(`${needle} not found`);
  return {line, character: lines[line]!.indexOf(needle) + offset};
}

/** The position at the very end of the document, where an author is typing. */
export function endOf(document: TextDocument): {line: number; character: number} {
  const lines = document.getText().split(/\r?\n/u);
  return {line: lines.length - 1, character: lines.at(-1)!.length};
}

export function uriPath(uri: string): string {
  return fileURLToPath(uri).replaceAll("\\", "/").toLowerCase();
}
