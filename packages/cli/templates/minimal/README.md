# Your first PrismatiX story

Run `prismatix dev .` here, or `prismatix dev "path/to/your-game"` from another directory. Edit `Story/main.pxstory` and save. Each successful update restarts the scene after the replacement Preview acknowledges the new revision. Compilation errors retain the previous preview and show the source line; correct the file and save again.

This short Traditional Chinese story includes a guide, a background, two choices and two endings. `--locale` chooses the project's locale tag; it does not machine-translate the story. Noto Sans TC covers the starter's Traditional Chinese text. Retain its license when distributing the game.

Start from the title screen. The default HUD supplies dialogue, choices, AUTO, SKIP, backlog, SAVE and LOAD. The title's Load button also opens saves. To customize the title, edit `Content/UI/Title.pxui`; custom HUD routes can be added to `prismatix.json` using the Authoring SDK.

Preview Save/Load slots are temporary checkpoints cleared by a rebuild or closing the Host. The packaged Player saves to disk and can load those saves after restarting.

Run `prismatix build .` to create a desktop distribution including Player. `--output` changes its location; use `--artifact-only` only when you explicitly need compiler output. Players do not need Node.js. Test the resulting Player before shipping.

`create` installs exact CLI and SDK versions and writes `package-lock.json`. Commit that lock with your source. After preparation, ordinary dev/build do not need network access. Keep existing `[id ...]` anchors when rewriting dialogue so edits can retain their identities; significant route, variable or resource changes require a tested save migration.

Add image files under `Assets`, register them in `prismatix.json`, and use their asset IDs in Story commands. UUIDs already present in the template are stable within this project; you do not need to replace them. Story command syntax differs from runtime JavaScript APIs. Runtime extensions use QuickJS and cannot import Node.js packages directly.

All starter pictures are original geometric artwork; see `ASSET_PROVENANCE.md`. This template contains no PrismaFamilyDemo character artwork.
