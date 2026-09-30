# PrismatiX Story for VS Code

Editor support for PrismatiX `.pxstory` Story files: highlighting, live
errors, completion, navigation and a "preview from this line" command. The
language server compiles Story with the same Authoring SDK as
`prismatix validate`, so what the editor reports is what the build reports.

## Install

The extension is not on the Marketplace yet. Build a `.vsix` from this
repository and install it:

```sh
npm ci
npm run build --workspace @prismatix/authoring-sdk
npm run build --workspace vscode-pxstory
npm run package --workspace vscode-pxstory   # needs registry access for @vscode/vsce
code --install-extension packages/vscode-pxstory/dist/vscode-pxstory.vsix
```

Or in VS Code: **Extensions** view → `…` menu → **Install from VSIX…** and
pick `dist/vscode-pxstory.vsix`. VS Code 1.91 or later is required.

## Features

**Highlighting.** Labels, commands (dotted names such as `bgm.stop` and
flow control such as `jump`/`if`), speaker lines (`@alias`, `@名前「…」`,
`@alias[options] text`), comments, inline text tags (`[b]`, `[ruby=…]`,
`[size=…]`, `[color=…]`, `[font=…]`), line continuations (`\`), strings,
numbers and durations (`1000ms`, `1.5s`, `150%`), macro definitions and
`%parameters`.

**Diagnostics.** The open file is compiled as you type, against the project
found by walking up to `prismatix.json`. Characters, the game catalog,
extensions, `storyAssets` and the Story index are the project's own, and
jumps into other scenes are checked against their current labels. Errors
carry the SDK's codes (`PXSTORY1305`, `PXSTORY1161`, …) and exact source
spans; a Story resource whose file is missing is reported as `PXBUILD1044`.
After a save, every scene of the project is compiled in the background, so a
renamed label also marks the jumps to it in closed files. On the 352-scene
*Sakura no Uta* port an edit re-checks in about 40 ms and the whole-project
check takes about 3 s on a worker thread.

**Completion.**

- Command names with their parameters (`[bg asset=… rule? duration? …]`) and
  a summary, then `name=` argument names.
- Character aliases and display names after `@`, in `[show]`, `[move]`,
  `[hide]` and `[act]`; expression aliases and positions for that character,
  including in `@alias[…]` options.
- `storyAssets` short names per kind: `[bg]` and `[image]` backgrounds,
  `rule=`, `[bgm]`, `[se]`, `[voice]` and `voice=`, `[ambience]`, `[video]`
  and `sprite=`. Each name is the shortest one the SDK's resolver maps back
  to that file, so `night/street` is offered when a bare `street` would pick
  another file. Projects without `storyAssets` get project paths instead.
- Labels of the current scene for `[jump]`, `[call]` and `[choice goto=]`;
  `scene:label` entries write `scene=… target=…` for another scene, and
  `scene=` completes scene ids.
- Macros defined in the file, game variables for `[set]`/`[if]`/`[add]`,
  image layer names, action presets, easing names and inline text tags.

**Go to definition** on a label reference (also across scenes), a `scene=`
id, a macro call, a character alias or display name (opens its
`.pxcharacter` at the alias), an expression alias, or a resource (opens the
file).

**Hover** shows command documentation, an image preview for backgrounds,
rules and character expressions (layered `.pxportrait` expressions show their
base and face layers), a character's display name, aliases and expressions,
and where a label target is defined.

**Outline and folding.** Labels and macros appear in the Outline view;
labels, macro bodies, `[if]…[endif]` blocks and comment runs fold.

## Commands

- **PrismatiX: Validate Project** runs the project's own
  `node_modules/@prismatix/cli` with `validate --json` and lists every
  diagnostic, including UI and packaging checks the editor does not run, in
  the Problems view. Extra flags (for example `--ui-registry …`) go in
  `prismatix.cli.validateArgs`.
- **PrismatiX: Preview From Here** (also in the editor context menu) saves
  the file and runs `prismatix dev <project> --at <scene>:<line>` for the
  cursor line. A CLI that does not support `--at` yet is reported in a
  notification with the CLI's own message. **PrismatiX: Stop Preview** (or
  the status bar item) closes the preview through the CLI's shutdown
  message, so its Preview host exits cleanly.

## Settings

| Setting | Default | |
| --- | --- | --- |
| `prismatix.languageServer.sdk` | `auto` | `auto` compiles with the project's installed `@prismatix/authoring-sdk` and falls back to the bundled SDK; `bundled` always uses the bundled one. |
| `prismatix.validation.projectOnSave` | `true` | Whole-project Story check after saving a `.pxstory`. |
| `prismatix.validation.debounceMs` | `250` | Delay before the open file is checked again. |
| `prismatix.cli.path` | | A CLI to run instead of the project's own (a `cli.js` or an executable). |
| `prismatix.cli.nodePath` | `node` | Node.js used to run the CLI. |
| `prismatix.cli.validateArgs`, `prismatix.cli.devArgs` | `[]` | Extra CLI arguments. |

The **PrismatiX Language Server** output channel logs which SDK a project
uses and how long loading and whole-project checks take.

## Limitations

- UI (TSX/`.pxui`), extension manifests and packaging are checked only by
  **Validate Project**, not live.
- Labels and macros are resolved per file; the SDK expands a macro body
  only where it is called, so errors inside a body point at the call.
- Adding or removing executable lines in a Story with a `.pxids.json`
  sidecar is reported at the `; @prismatix-ids` marker until
  `prismatix ids <project> --sync --write` updates the sidecar, as in the
  build.

## Development

```sh
npm run build --workspace vscode-pxstory   # tsc (out/) and esbuild bundles (dist/)
npm test --workspace vscode-pxstory        # grammar snapshot and language server tests
UPDATE_SNAPSHOTS=1 npm test --workspace vscode-pxstory
```

The server can also run outside VS Code: `node dist/server.js --stdio`.
