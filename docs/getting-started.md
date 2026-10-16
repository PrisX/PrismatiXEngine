# Getting started

Make a small visual novel with PrismatiX, from an empty folder to a game you
can send to someone. The [Story reference](story-reference.md), the
[UI reference](ui-reference.md) and the [shipping reference](shipping.md)
cover each part in full.

- [Install and create a project](#install-and-create-a-project)
- [Edit the story while it plays](#edit-the-story-while-it-plays)
- [Write your first scenes](#write-your-first-scenes)
- [Add characters, scenes and screens](#add-characters-scenes-and-screens)
- [Build a game to share](#build-a-game-to-share)
- [When something goes wrong](#when-something-goes-wrong)

## Install and create a project

Install Node.js (24 or later) and npm first. The CLI package carries the
native Player and Packager for your platform; you do not need C++, CMake or an
engine checkout.

```bash
npm install -g @prismatix/cli
prismatix create my-game
prismatix dev my-game
```

Run `prismatix create` without a folder to be asked for the title, folder,
template, resolution, story language and package manager instead. `prismatix`
on its own opens a menu: inside a project it offers preview, checks, builds,
tests and adding characters or scenes; elsewhere it offers to create a game.
Prompts appear only in a terminal; scripts, CI and `--json` never wait for
input, and options you pass are not asked again.

With pnpm, `pnpm add -g @prismatix/cli` (or `pnpm dlx @prismatix/cli create my-game`)
works the same way. `create` installs the project's tools with the package
manager that ran it, or with the one `--package-manager npm|pnpm` names.

`create` prepares a short Traditional Chinese story with original character/background pictures, two choices and endings, a title, default dialogue HUD and save/load routes. It copies Noto Sans TC and its license, prepares editor settings, installs exact project CLI/SDK versions and writes `package-lock.json` (npm) or `pnpm-lock.yaml` (pnpm, which also records its version in `packageManager`). No npm init, second SDK install, hand-written UUIDs or initialization command is needed. Initial preparation needs registry access; failures preserve existing author data.

All options are non-interactive:

```bash
prismatix create "my game" --name "First Light" --id first-light --locale zh-TW --width 1280 --height 720
```

`--locale` sets the locale tag; it does not translate the Chinese template. The bundled font covers the starter text. Add and test appropriate fonts before authoring other writing systems.

## Edit the story while it plays

Edit `my-game/Story/main.pxstory` and save: `dev` rebuilds and restarts the
Preview at the scene you were in. A syntax error keeps the last good version
playing and shows the error in the terminal, so fix it and save again. To work
deep inside a long scene, start Preview there and keep it there across edits:

```bash
prismatix dev my-game --at chapter2:120
prismatix dev my-game --at chapter2:after_school
```

A line number or a label works; Preview replays the story up to that point, so
backgrounds, characters and variables are as they would be. Press Ctrl+C to
stop. Preview starts at the title; its Save and Load slots last only until the
next rebuild, while a packaged game keeps real saves.

The story has no `[id]` lines in it: `create` keeps the save identities of its
lines in the `.pxids.json` file beside it. Keep the `; @prismatix-ids` line at
the top and that file together; [Story reference: identities](story-reference.md#keeping-saves-working-while-you-edit)
explains what to do after adding or removing lines.

### Editing Story in VS Code

The `vscode-pxstory` extension in `packages/vscode-pxstory` highlights
`.pxstory` files and runs a language server built on the same Authoring SDK
as `validate`: errors appear as you type (with the SDK's codes and spans),
completion offers commands, characters and expressions, `storyAssets` short
names and labels (including `scene:label` in other scenes), and
go-to-definition and hover work on labels, macros, characters and images.
**PrismatiX: Validate Project** runs the project's own CLI, and **PrismatiX:
Preview From Here** starts `prismatix dev` at the cursor's scene and line
(`--at <scene>:<line>`; a CLI without that flag is reported, not ignored).
It is not on the Marketplace yet; build and install the `.vsix` as described
in [its README](../packages/vscode-pxstory/README.md).

## Write your first scenes

```pxstory
*start
@guide
今天想去哪裡呢？

[choice]
- 看看星星 -> stars
- 聽個故事 -> story
[endchoice]
*stars
第一顆星亮起了。
[jump finish]
*story
很久很久以前，有一個小小的願望。
*finish
[end]
```

A name after `@` speaks; an empty line returns to narration. Labels are
`*name`, each `- text -> label` line in a `[choice]` block is one choice, and
`[jump finish]` continues at a label. The starter also shows a background and
a character:

```pxstory
[bg room-dusk]
[show guide smile left]
@guide 這裡就是新的背景。
[move guide position=right duration=400]
[hide guide]
```

Variables need no setup beyond a declaration:

```pxstory
[var affection=0]
[set affection=1]
[if affection > 0]
小光記得這個約定。
[endif]
```

The [Story reference](story-reference.md) lists every command: sound, image
layers, transitions, text styles, ruby, macros, NVL pages, credits and more.

## Add characters, scenes and screens

Scaffold the files you would otherwise write by hand; any command explains its
options with `--help`:

```bash
prismatix add character kei --from Assets/Portraits/kei --name 圭
prismatix add scene chapter2 --chapter chapter-1 --title "放學後"
prismatix eject ui hud
prismatix release --help
```

`add character` makes `normal.png` (or `default`/`neutral`) the default
expression and, without `--from`, registers a character with no sprite, such
as a first-person protagonist who still needs a name.

`add scene` writes the scene for every locale, lists it in a chapter of the
story index and ends it with `[jump scene=next]`, which continues with the
following scene of the index (see [scenes](story-reference.md#scenes-and-the-story-index)).

Writers who draft in Markdown can keep doing so: `prismatix import
screenplay` turns the draft into scenes and can be re-run after every edit
(see [importing a writers' draft](story-reference.md#importing-a-writers-draft)).

`prismatix create my-game --template adv` starts with the HUD, Save, Load,
Settings and Backlog already ejected this way. `eject ui <route>` (or `all`) copies a stock Player screen (`title`, `hud`,
`backlog`, `save`, `load`, `gallery`, `settings`, `video`) into
`Content/UI/<Route>.tsx`, scaled to the project's resolution, and registers it
in `uiEntryPoints`. It lowers to exactly the built-in screen, so you start from
a working HUD or Save screen with every control name and binding the Player
needs, and restyle from there.

The [UI reference](ui-reference.md) covers TSX screens: buttons and panels,
the data the Player publishes, settings, save management and input.

## Build a game to share

```bash
prismatix dev my-game
prismatix validate my-game
prismatix build my-game
prismatix build my-game --output "release/First Light"
```

`build` creates a desktop distribution with Player, game content and redistributable dependencies in `my-game/dist/<project-id>/`. Players need neither Node.js nor the SDK. `--output` changes only the location. `--artifact-only` writes just the compiled game, for a later `pack`:

```bash
prismatix build my-game --artifact-only --output my-game/.prismatix/artifact
```

For a release, `release` validates, builds, packs and verifies everything in
one step:

```bash
prismatix release my-game
```

Then commit `.prismatix-release/lineage.json` so later releases keep loading
players' saves. Settings, smaller media, updates, patches and automated play
tests are in the [shipping reference](shipping.md).

## When something goes wrong

The terminal shows readable diagnostics by default. Pass `--json` for stable NDJSON codes, paths and source spans on stderr; preserve stderr for editor integrations. `PXSTORY1199` identifies an unknown Story command. Unknown options and invalid typed values fail. `dev --once` tests initial apply/play and shutdown, not an interactive editing session.

Normal discovery uses the matching `runtime/<platform>-<architecture>/` bundle inside the CLI package. A missing or incompatible bundle is an installation/release problem. Do not install C++ build tools as an author workaround. Explicit `--runtime`, per-tool options and `PRISMATIX_*` overrides are developer diagnostics.

For registry or proxy errors during create, fix npm's registry/proxy configuration and retry. Do not use sudo or disable security controls. A nonempty or read-only destination is rejected. After abrupt termination, inspect a leftover create lock before removing its empty lock directory and retrying.
