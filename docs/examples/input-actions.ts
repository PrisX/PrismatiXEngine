import {validateDocument, type GameDocument} from "@prismatix/authoring-sdk";

const game: GameDocument = {
  format: "PrismatiXGame", schemaRevision: 2,
  variables: [], gallery: [], unlockables: [],
  inputActions: [
    {id: "quick-save", context: "game", keys: ["F8"]},
    {id: "screen.open.gallery", context: "game", keys: ["G"], route: "gallery"},
    {id: "photo-mode", context: "game"},
  ],
};
const result = validateDocument("game", game);
if (!result.valid) throw new Error(JSON.stringify(result.diagnostics));
process.stdout.write("Named input action documentation example passed\n");
