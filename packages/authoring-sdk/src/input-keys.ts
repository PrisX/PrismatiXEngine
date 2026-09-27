import {jsonPointerSpans} from "./json-source-spans.js";
import type {AuthoringDiagnostic, GameDocument, SourceSpan} from "./types.js";

/** Key names exported from the selected native Runtime's Input::KeyCode table. */
export interface InputKeyRegistryMetadata {
  readonly format: "PrismatiXInputKeyRegistry";
  readonly schemaRevision: 1;
  readonly keys: readonly string[];
}

function nearKeyName(name: string, keys: readonly string[]): string | undefined {
  const source = name.toLocaleLowerCase("en-US");
  let bestDistance = 3;
  let best: string | undefined;
  for (const candidate of keys) {
    const target = candidate.toLocaleLowerCase("en-US");
    if (Math.abs(source.length - target.length) > 2) continue;
    let previous = Array.from({length: target.length + 1}, (_, index) => index);
    for (let row = 1; row <= source.length; row += 1) {
      const current = [row];
      for (let column = 1; column <= target.length; column += 1)
        current.push(Math.min(current[column - 1]! + 1, previous[column]! + 1,
          previous[column - 1]! + (source[row - 1] === target[column - 1] ? 0 : 1)));
      previous = current;
    }
    const distance = previous[target.length]!;
    if (distance < bestDistance) {bestDistance = distance; best = candidate;}
  }
  return best;
}

export function validateInputKeyUsage(game: GameDocument, path: string,
  registry: InputKeyRegistryMetadata, source?: string): AuthoringDiagnostic[] {
  // The manifest contains spellings verified by Input::KeyCode, including the
  // case-sensitive historical "Enter" alias. Do not infer other spellings.
  const accepted = new Set(registry.keys);
  const diagnostics: AuthoringDiagnostic[] = [];
  let spans: ReadonlyMap<string, SourceSpan> | undefined;
  const check = (key: string, owner: string, pointer: string): void => {
    if (accepted.has(key)) return;
    const suggestion = nearKeyName(key, registry.keys);
    const span = (spans ??= jsonPointerSpans(source, game, path)).get(pointer);
    diagnostics.push({severity: "error", code: "PXBUILD1068",
      message: "Input key name is not recognized by the selected native Runtime",
      path, details: `${owner}: ${key}`,
      ...(span === undefined ? {} : {span}),
      hint: suggestion === undefined
        ? "Use a key name from PrismatiXInputKeyRegistry.json beside the selected Runtime."
        : `Did you mean "${suggestion}"? Use a spelling from PrismatiXInputKeyRegistry.json.`});
  };
  for (const [actionIndex, action] of (game.inputActions ?? []).entries())
    for (const [keyIndex, key] of (action.keys ?? []).entries())
      check(key, `inputActions.${action.id}`,
        `/inputActions/${actionIndex}/keys/${keyIndex}`);
  for (const [index, entry] of (game.inputBindings ?? []).entries()) {
    if (entry === null || typeof entry !== "object" || Array.isArray(entry)) continue;
    const key = entry.key;
    if (typeof key === "string") check(key, `inputBindings[${index}]`,
      `/inputBindings/${index}/key`);
  }
  return diagnostics;
}
