import {validateDocument, type GameDocument} from "@prismatix/authoring-sdk";

const game: GameDocument = {
  format: "PrismatiXGame", schemaRevision: 1,
  variables: [], gallery: [], unlockables: [],
  inputActions: [
    {id: "quick-save", context: "game", keys: ["F8"]},
    {id: "screen.open.gallery", context: "game", keys: ["G"], route: "gallery"},
    {id: "backlog", context: "game", wheel: {direction: "up", threshold: 1}},
    {id: "advance", context: "game", wheel: {direction: "down"}},
    {id: "photo-mode", context: "game"},
    {id: "settings.keys", context: "ui", keys: ["F2"],
      uiAction: {command: "settings.page7"}},
  ],
};
const result = validateDocument("game", game);
if (!result.valid) throw new Error(JSON.stringify(result.diagnostics));
process.stdout.write("Named input action documentation example passed\n");
