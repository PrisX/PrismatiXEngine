import type {AuthoringDiagnostic, StoryDocument, StoryNode} from "./types.js";

function controlSignature(node: StoryNode): string | undefined {
  if (node.kind === "label") return `label:${node.name ?? ""}`;
  if (node.kind !== "command") return undefined;
  if (!["jump", "call", "return", "end", "choice", "choice.wait", "if", "else", "endif"].includes(node.name ?? "")) return undefined;
  const identity = (node.arguments ?? []).find((argument) => argument.name === "id" || argument.name === "goto" || argument.name === "target");
  return `${node.name}:${identity?.value === undefined ? "" : String(identity.value)}`;
}

export function validateStoryLocalizationStructure(reference: StoryDocument, localized: StoryDocument): readonly AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [...localized.diagnostics];
  const expected = reference.nodes.map(controlSignature).filter((value): value is string => value !== undefined);
  const actual = localized.nodes.map(controlSignature).filter((value): value is string => value !== undefined);
  if (expected.length !== actual.length) {
    diagnostics.push({severity: "error", code: "PXLOC1001", message: "Localized Story has a different control-flow structure", path: localized.path, details: `expected ${expected.length} structural nodes, received ${actual.length}`});
    return diagnostics;
  }
  for (let index = 0; index < expected.length; index += 1) {
    if (expected[index] === actual[index]) continue;
    diagnostics.push({severity: "error", code: "PXLOC1002", message: "Localized Story control-flow identity differs", path: localized.path, details: `at structural node ${index}: expected ${expected[index]}, received ${actual[index]}`});
  }
  return diagnostics;
}
