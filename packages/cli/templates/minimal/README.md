# Your first PrismatiX story

Run `prismatix dev .` here, or `prismatix dev "path/to/your-game"` from another directory. Edit `Story/main.pxstory` and save. Each successful update restarts the scene after the replacement Preview acknowledges the new revision. Compilation errors retain the previous preview and show the source line; correct the file and save again.

This short Traditional Chinese story includes a guide, a background, two choices and two endings. `--locale` chooses the project's locale tag; it does not machine-translate the story. Noto Sans TC covers the starter's Traditional Chinese text. Retain its license when distributing the game.

The game opens with a short "Made with PrismatiX" startup screen (`Content/UI/Splash.tsx`, listed in `player.splashes`); a click skips it. Edit it in PrismatiX Studio's Screens workspace, replace it with your own logo, or remove it from Game flow (or delete the `player.splashes` entry). `Content/UI/PrismatiX/NOTICE.md` says how its logo may be used.

Start from the title screen. The default HUD supplies dialogue, choices, AUTO, SKIP, backlog, SAVE and LOAD. The title's Load button also opens saves. To customize the title, edit `Content/UI/Title.tsx` (positions there are fractions of the resolution); custom HUD routes can be added to `prismatix.json` using the Authoring SDK.

Preview Save/Load slots are temporary checkpoints cleared by a rebuild or closing the Host. The packaged Player saves to disk and can load those saves after restarting.

Run `prismatix build .` to create a desktop distribution including Player. `--output` changes its location; use `--artifact-only` only when you explicitly need compiler output. Players do not need Node.js. Test the resulting Player before shipping.

`create` installs exact CLI and SDK versions and writes `package-lock.json`. Commit that lock with your source. After preparation, ordinary dev/build do not need network access. The Story needs no hand-written IDs: `create` keeps stable save identities in `Story/main.pxstory.pxids.json`. Keep that file and the Story's `; @prismatix-ids` marker together. Prose rewrites keep their identities. After adding or removing executable lines, run `prismatix ids . --sync` to review changes and `prismatix ids . --sync --write` to update the sidecar. If rewriting a line and inserting immediately beside it, sync the rewrite before inserting; otherwise the old/new identity can be ambiguous. Test old saves before shipping significant route, variable or resource changes.

Add image files under `Assets` and name them in Story commands: `storyAssets` in `prismatix.json` lets `[bg room-dusk]` find `Assets/room-dusk.jpg` (a file under `Assets` needs an `assets` entry or a `resourceIncludes` root to be packaged), and a full project-relative path such as `[bg Assets/room-dusk.jpg]` always works. Add a character expression with `assetPath` in `Characters/guide.pxcharacter`; Authoring generates its canonical asset, character and expression UUIDs. Keep its `source` path and expression alias stable across released save versions, or explicitly pin IDs before renaming them. Other UUIDs already present in the template are stable within this project; you do not need to replace them. Story command syntax differs from runtime JavaScript APIs. Runtime extensions use QuickJS and cannot import Node.js packages directly.

The starter's backgrounds and character were generated for PrismatiX with the Anima model and may be kept or replaced; `ASSET_PROVENANCE.md` records how. This template contains no PrismaFamilyDemo character artwork.
