# @prismatix/cli

The `prismatix` command creates, previews, validates, builds and packs
[PrismatiX](https://github.com/PrisX/PrismatiXEngine) visual novels. The
native PrismatiX Player, Packager and Preview Host for your platform come with
it (see [Native runtimes](#native-runtimes)), so you do not need C++ tools or
an engine checkout. Players of the finished game do not need Node.js.

## Install

Node.js 22.15 or later and pnpm (`corepack enable pnpm`, or
https://pnpm.io/installation):

```bash
pnpm add -g @prismatix/cli
prismatix create my-game
prismatix dev my-game
```

`pnpm dlx @prismatix/cli create my-game` creates a project without a global
install. npm works too: `npm install -g @prismatix/cli` and
`prismatix create my-game --package-manager npm`.

Each project pins `@prismatix/cli`, `@prismatix/authoring-sdk` and
`@prismatix/runtime` to the same exact version in its `package.json`. The
global command hands project commands to that pinned local CLI, so commit the
lockfile with your source and upgrade deliberately.

## Commands

| Command | What it does |
| --- | --- |
| `create [folder]` | A new project with a starter story, title screen and pinned tools (asks in a terminal) |
| `dev [project]` | Live Preview that replays after every saved edit; `--at chapter2:120` starts at a line or label |
| `validate [project]` | Checks Story, UI, characters, resources and extensions |
| `build [project]` | A desktop distribution including the Player |
| `pack [project]` | Packs content with the Player; `--platform` packs for other desktop platforms; `--target web` and `--target android` (experimental) |
| `release [project]` | Validate, build, pack and verify in one step |
| `add character`, `add scene` | Adds project content |
| `add platform <platform>` | Adds another desktop platform's runtime (this CLI's exact version, integrity-checked) for `pack --platform`; `--check` lists them |
| `add web-player` | Adds `@prismatix/web-player` (this CLI's exact version) for `pack --target web`; `--check` only reports |
| `extension create <id>` | A TypeScript (or `--lang js`) runtime extension |
| `eject ui <route>` | Copies a built-in screen into the project as TSX |
| `import artemis <extracted> [project]` | Converts an Artemis game |
| `ids --sync` | Reviews stable save identities after adding or removing Story lines |
| `create-patch`, `create-overlay` | Signed single-file updates for a shipped game |
| `test` | Runs `tests/journeys/*.ts` against a QA pack |

`prismatix <command> --help` explains each command. `--json` prints stable NDJSON
diagnostics for editors and CI.

## Native runtimes

Each platform's runtime is its own package. This CLI lists all of them as
optional dependencies of exactly its own version; pnpm and npm install only
the one whose `os` and `cpu` match your machine, and the CLI finds it the way
Node.js resolves its dependencies.

| Package | Platform |
| --- | --- |
| `@prismatix/native-win32-x64` | Windows x64 |
| `@prismatix/native-linux-x64` | Linux x64 (glibc) |
| `@prismatix/native-darwin-x64` | macOS Intel |
| `@prismatix/native-darwin-arm64` | macOS Apple silicon |

Installing with `--no-optional` or `--omit=optional` leaves the runtime out;
the CLI then names the missing package. On any other platform the CLI says
there is no native runtime for it.

Each native package carries its own `share/PrismatiX/runtime-manifest.json`,
the MIT license, `THIRD_PARTY_NOTICES.md`, `ASSET_PROVENANCE.json` and the
licences of its native dependencies; a packed game copies them next to the
Player under `Notices/`. Which platforms are verified for a release is listed in
its release notes.

The experimental Web Player (`pack --target web`) is the separate package
`@prismatix/web-player`, which this CLI does not install. Add it to a project
that exports for the web with `prismatix add web-player`, or
`pnpm add -D @prismatix/web-player@<this CLI's version>`.

## Documentation

- [Getting started](https://github.com/PrisX/PrismatiXEngine/blob/master/docs/getting-started.md)
- [Story reference](https://github.com/PrisX/PrismatiXEngine/blob/master/docs/story-reference.md)
- [UI reference](https://github.com/PrisX/PrismatiXEngine/blob/master/docs/ui-reference.md)
- [Shipping a game](https://github.com/PrisX/PrismatiXEngine/blob/master/docs/shipping.md)
- [Release notes](https://github.com/PrisX/PrismatiXEngine/tree/master/docs/releases)

## License

MIT. The native runtime packages include third-party components under their own
licences; see `share/PrismatiX/THIRD_PARTY_NOTICES.md` in the
`@prismatix/native-<platform>-<architecture>` package.
