# Studio extensions

An extension can add to PrismatiX Studio as well as to the game. One
`.pxextension` manifest (and one `.pxext` package) holds up to two sides:

- the **game side**: `entry` and the fields beside it (`language`,
  `modules`, `capabilities`, `native`, `platforms`, `safety`, `commands`,
  `actions`). The Player runs it in the game's sandbox, as described in
  [Extension commands](story-reference.md#extension-commands).
- the **Studio side**: the `studio` object. Studio loads it in a sandboxed
  frame while you edit the project, to classify asset files, show an
  inspector for an asset or offer choices for a command's parameter. It never
  reaches a game.

An extension has at least one side. Both share one id, one version, one lock
entry, one package and one install consent.

- [Creating one](#creating-one)
- [The manifest](#the-manifest)
- [Parameter choices](#parameter-choices)
- [Building and packing](#building-and-packing)
- [How Studio loads it](#how-studio-loads-it)
- [Games](#games)
- [Diagnostics](#diagnostics)

## Creating one

```sh
prismatix extension create live2d --side both     # a game side and a Studio side
prismatix extension create tools --side studio    # a Studio-only extension
prismatix extension build                         # rebuild every Studio side from its TypeScript
```

`--side both` writes the usual game files and `studio.ts` with its own
`tsconfig.studio.json` (the game's `tsconfig.json` leaves it out, because the
game's sandbox has no DOM and the Studio side's frame does). The Studio side
starts as an inspector for text files. `create` bundles `studio.ts` into
`dist/studio.js`, the entry the manifest names; run `prismatix extension
build` after you change it. With `--lang js` the Studio side is `studio.js`,
loaded as written.

`extension list` prints each extension's sides (`--json`: `"sides":
["game", "studio"]`), and `add-command` and `add-action` refuse a Studio-only
extension.

## The manifest

The Studio side needs `"schemaRevision": 2`. Revision 1 manifests are
unchanged and cannot have `studio`; the CLI says to raise the revision when
one does (`PXSDKSEM1126`). An engine or CLI from before revision 2 rejects a
revision-2 manifest with its ordinary schema error.

```json
{
  "format": "PrismatiXExtension",
  "schemaRevision": 2,
  "id": "live2d",
  "version": "1.0.0",
  "requiredEngineVersion": ">=0.1.0 <0.2.0",
  "studio": {
    "entry": "dist/studio.js",
    "requiredStudioVersion": ">=0.1.0",
    "permissions": ["project.read"],
    "contributes": {
      "assetTypes": [
        {"id": "live2d.model", "displayName": "Live2D model", "patterns": ["*.model3.json"]}
      ],
      "inspectors": [
        {"assetType": "live2d.model", "displayName": "Live2D preview"}
      ],
      "parameterOptions": [
        {"id": "motions", "displayName": "Motions"}
      ]
    }
  }
}
```

This one is Studio-only: it has no `entry`. Without `entry` a manifest may
not have `modules`, `native`, `platforms` or `safety`, and `capabilities`,
`commands` and `actions` are absent or empty (`PXSDKSEM1128`); `language` may
stay. A revision-2 manifest with `entry` is a revision-1 manifest plus
`studio`: `language`, `capabilities`, `commands` and `actions` are required
as before.

`studio`:

| Field | |
|---|---|
| `entry` | Required. Built ES module JavaScript (`.js`), relative to the manifest. |
| `requiredStudioVersion` | Required. A SemVer range of Studio versions, in the same grammar as `requiredEngineVersion`. Studio skips a Studio side that does not match its version. |
| `permissions` | What the Studio side may do; default none. `project.read` reads files inside the project, `project.write` creates and overwrites files inside the project. |
| `contributes` | What it adds to Studio (below); each list holds at most 64 items. |

There is nothing else to ask for: a Studio side has no network, no
processes, no native code and no files outside the project.

`contributes`:

- `assetTypes`: `{id, displayName, description?, patterns}`. Studio's asset
  browser shows a file whose name matches one of the 1 to 16 `patterns` as
  this type (its label and filter) without running any code. A pattern is a
  file-name glob with `*` and `?`, matched case-insensitively against the
  name only (`*.model3.json`, not `models/*.json`). Ids are unique within the
  extension.
- `inspectors`: `{assetType, displayName?, description?}`. The Studio side
  draws the inspector of an asset of this type in the Assets workspace.
  `assetType` is one of the extension's own `assetTypes` or a built-in kind:
  `image`, `audio`, `video`, `font` or `text`. At most one inspector per
  asset type.
- `parameterOptions`: `{id, displayName?, description?}`. A provider of
  choices for a command or Action parameter (see `optionsFrom` below).

A contribution's full id is `<extension id>:<contribution id>`, for example
`owen/live2d:motions` for an installed package or `live2d:motions` for a
project's own extension without an owner.

## Parameter choices

Two parameter fields help Studio's command and Action editors. Both work in
revision 1 and 2 manifests.

`resourceFilter` (type `resource`) names the files the picker offers: a
media kind (`image/*`, `audio/*`, `video/*`, `font/*`, `text/*` or an exact
`type/subtype` such as `audio/ogg`), a file-name glob (`*.model3.json`) or a
comma-separated list of them (`image/*, *.model3.json`).

`optionsFrom` (types `string` and `token`, not together with `enum`) makes
the editor a dropdown:

```json
{"name": "model", "type": "resource", "resourceFilter": "*.model3.json"},
{"name": "motion", "type": "string",
 "optionsFrom": {"parameter": "model", "pointer": "/FileReferences/Motions", "take": "keys"}},
{"name": "expression", "type": "token", "optionsFrom": {"provider": "owen/live2d:expressions"}}
```

- `{"parameter", "pointer", "take"}`: the choices come from the JSON file the
  sibling `resource` parameter names, at the RFC 6901 `pointer` (`""` is the
  whole document). `take` is `keys` (an object's keys, the default) or
  `values` (array items, or an object's values, that are strings or numbers).
- `{"provider"}`: the choices come from a Studio side's `parameterOptions`
  contribution, by its full id. It may be another extension's.

Only Studio uses `optionsFrom`. The game treats the value as an ordinary
string or token and never checks it against the choices, so a value typed
by hand still works. The command schema the SDK hands editors
(`storyCommandSchema`) carries each parameter's `optionsFrom`.

## Building and packing

`studio.entry` is JavaScript. When a TypeScript source sits next to it
(`studio.ts` for `studio.js`, or `studio.ts` beside the manifest for
`dist/studio.js`), `prismatix extension build` and `prismatix extension
pack` bundle it with everything it imports into one ES module for a browser
frame, as a game build bundles a TypeScript `entry`. A Node.js import stops
the build (`PXCLIEXT1001`). Without a source, the `.js` file is used as it
is.

`pack` puts the Studio entry in the package's `dist/` (`dist/studio.js`) and
the manifest points at it there; a Studio-only package has no
`types/prismatix-extension.d.ts`. The package's SHA-256, which the lock
records, covers the Studio entry like every other file. Everything in
[Sharing extensions](extensions-sharing.md) applies unchanged: publishing,
installing, the lock, `verify`.

`extension inspect` and the confirmation before `extension add` show the
Studio side's permissions and contributions and say when a package adds
nothing to the game. With `--json`, `inspect` adds

```json
"sides": ["game", "studio"],
"studio": {
  "requiredStudioVersion": ">=0.1.0",
  "permissions": ["project.read"],
  "contributes": {
    "assetTypes": [{"id": "live2d.model", "displayName": "Live2D model"}],
    "inspectors": [{"assetType": "live2d.model"}],
    "parameterOptions": [{"id": "motions", "displayName": "Motions"}]
  }
}
```

and `extension list --json` adds `sides` to each extension. `update` asks
again when a new version asks for a Studio permission the installed one did
not. A registry may announce `sides` and `requiredStudioVersion` on search
results and extension documents; both are optional, and without `sides` an
extension has a game side only.

## How Studio loads it

This is what Studio does with an installed or project-local extension that
has a Studio side and a matching `requiredStudioVersion`:

- It loads `studio.entry` in a sandboxed iframe (`sandbox="allow-scripts"`,
  without `allow-same-origin`) under a Content Security Policy with no
  network access. Studio reads the file and hands its text to the frame; the
  frame never sees Studio's own connections.
- The module exports `activate(studio)`. `studio.inspectors.register`,
  `studio.parameterOptions.register` and `studio.project` (`readFile`,
  `readText`, `list` with `project.read`; `writeFile` with `project.write`)
  are the whole API; the types are in `@prismatix/studio-sdk`. Paths are
  project-relative; `..` and absolute paths are refused, and a call without
  its permission fails with an error that names the permission. `writeFile`
  never writes what would run outside the sandbox: hidden files and folders
  (`.git`, `.vscode`, ...), `NativeUserFiles/`, `node_modules/`,
  `package.json`, executables and native libraries.
- Asset types work without loading the module: the asset browser matches
  file names against `patterns`.
- Installing is not enough. Because the lock is shared through git, Studio
  asks each user before it first loads an extension's Studio side, for that
  extension and that set of permissions, and remembers the answer in the
  user's Studio settings. A version that asks for more permissions asks
  again. Declining leaves the Studio side unloaded; the game side is not
  affected.
- Errors an extension throws are shown in the Extensions workspace with its
  state (loaded, not allowed, incompatible Studio version, error) and never
  stop Studio.

Version 1 has inspectors, asset types and parameter choices only: no panels,
importers, validators or menu commands, no channel to the preview Player, no
user-level installs, no native code and no network.

## Games

A game uses only game sides. `validate`, `build`, `run` and `pack` of a
game skip a Studio-only extension entirely: it adds no commands, Actions,
files or Runtime IR, and the game's `project.pxproject` does not list it. A
manifest with both sides reaches the Player as a revision-1 manifest without
`studio`, and the Studio side's files never go into a game. A Studio-only
manifest must still match its contract, so a broken one fails `validate`.

## Diagnostics

| Code | Meaning |
|---|---|
| PXSDKSEM1126 | `studio` in a revision-1 manifest: set `schemaRevision` to 2 |
| PXSDKSEM1127 | A revision-2 manifest with neither `entry` nor `studio` |
| PXSDKSEM1128 | A Studio-only manifest with a game field (`modules`, `native`, `platforms`, `safety`, or non-empty `capabilities`, `commands`, `actions`) |
| PXSDKSEM1129 | `studio.requiredStudioVersion` is not a SemVer range |
| PXSDKSEM1130 | Two `assetTypes` or two `parameterOptions` share an id |
| PXSDKSEM1131 | An inspector names an asset type the extension does not declare and that is not built in |
| PXSDKSEM1132 | Two inspectors for one asset type |
| PXSDKSEM1133 | `optionsFrom` on a parameter that is not `string` or `token`, or together with `enum` |
| PXSDKSEM1134 | `optionsFrom.parameter` does not name a `resource` parameter of the same command or Action |
| PXSDKSEM1135 | `optionsFrom.pointer` is not a JSON pointer |
| PXSDKSEM1136 | `optionsFrom.provider` is not `<extension id>:<id>` |
| PXSDKSEM1137 | `optionsFrom.provider` names this extension but a `parameterOptions` id its Studio side does not contribute |
| PXSDKSEM1138 | A `resourceFilter` item is neither a media kind nor a file-name glob |

Packages report a Studio entry outside `dist/`, not `.js`, or missing as
`PXEXTPKG1010`.
