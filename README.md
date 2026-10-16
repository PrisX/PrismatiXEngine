<p align="center">
  <img src="https://cdn.owen0924.com/images/PrismatiXEngine_Logo.png" alt="PrismatiXEngine Logo">
</p>

<h1 align="center">PrismatiXEngine</h1>

<p align="center">
  <strong>A visual novel engine: a native C++ Player, a TypeScript authoring toolchain, and sandboxed JavaScript extensions.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=flat-square&logo=c%2B%2B" alt="C++20">
  <img src="https://img.shields.io/badge/JavaScript-QuickJS--NG-f7df1e.svg?style=flat-square&logo=javascript" alt="QuickJS-NG">
  <img src="https://img.shields.io/badge/Render-SDL3-green.svg?style=flat-square&logo=sdl" alt="SDL3">
  <img src="https://img.shields.io/badge/License-MIT-lightgrey.svg?style=flat-square" alt="MIT License">
</p>

You write the story in `.pxstory` scripts and the screens in TSX. The CLI checks
the whole project, compiles it to Runtime IR, and packs it with the native Player
into a game you can ship. Saves stay loadable across releases because every
executable line has a stable ID.

## From a new project to a release

```bash
npm install -g @prismatix/cli                 # or: pnpm add -g @prismatix/cli
prismatix create                              # asks for title, folder, template, resolution...
prismatix create my-game --template adv       # or non-interactive: editable HUD, save/load, settings and backlog
prismatix dev my-game                         # live Preview; --at chapter2:120 starts at a line or label
prismatix add character hero --from art/hero --project my-game
prismatix add scene chapter2 --project my-game
prismatix validate my-game
prismatix release my-game                     # validate, build, pack and verify in one step
```

Each command explains itself with `prismatix <command> --help`. Others you will use:

| Command | What it does |
| --- | --- |
| `eject ui <route>` | Copies a built-in screen (title, hud, save, settings...) into the project as TSX |
| `import artemis <extracted> [project]` | Converts an Artemis game; game-specific tables go in `artemis-import.json` |
| `pack --target android` | Builds an APK and AAB with the Android Player (experimental, see the shipping guide) |
| `pack --target web --web-player <dir>` | Exports the full WASM Player, chunked content, MP4 movies and browser persistence |
| `ids --sync` | Reviews save identities after adding or removing Story lines |
| `create-patch`, `create-overlay` | Signed single-file updates for a shipped game |
| `test` | Runs `tests/journeys/*.ts` against a QA pack (see below) |

The `@prismatix/*` packages are release candidates and are not on the public npm
registry yet. The [delivery contract](docs/releases/delivery-contract.md) lists the
supported targets.

## Story at a glance

```pxstory
*top
[var player=Alex type=string]
[bg room_day]
[show hero smile left]
@hero Good morning.
[input var=player prompt="Your name?"]
@hero Nice to meet you, {player}.

[choice]
- Go outside -> outside
- Stay in -> stay
[endchoice]
```

Shared macros, `[if]`/`[elseif]`, conditional and block choices, inline
`[w]`/`[speed]`, `[face]`, `[credits]` and the rest are in the
[Story reference](docs/story-reference.md). The VS Code extension in
`packages/vscode-pxstory` gives live errors, completion and go-to-definition.

## Screens at a glance

```tsx
<TextureButton name="Save" skin="ui/save" action={actions.screen.save} />
<Slider name="TextSpeed" option="mspeed" />
<Toggle name="Fullscreen" on="ui/fs-on" off="ui/fs-off" checked="settings.fullscreen"
  action={actions.setting.toggleFullscreen} />
<ItemsPlaced name="ChoiceList" items={{path: "dialogue.choices", key: "key", placement: "rect"}}
  choiceLayout={{area: [0, 497, 1920, 86], pitch: 60}} />
```

The `actions.*` builders are typed against the Player's action table, so a
misspelled action or a wrong argument fails `validate`. Screen states that depend
on what the player has cleared are rules in `prismatix.json`
(`player.profileUiData`), not code. `distribution.transcode` stores PNG as WebP
and Ogg as Opus in builds, which cut Sakura no Uta's media from 7.6 to 2.1 GiB.

## Guides

- [Getting started](docs/getting-started.md): install, create a project, edit
  it while it plays, build a game to share.
- [Story reference](docs/story-reference.md): Story scripts, text, characters,
  sound, variables, scenes, importing drafts, layered portraits and motion.
- [UI reference](docs/ui-reference.md): TSX screens, Player actions and data,
  settings, save management, input, custom effects.
- [Shipping reference](docs/shipping.md): distributions and releases, saves
  across versions, patches and overlays, user data, play tests, importers,
  Android.
- [Web export](docs/web-export.md): build the shipping WASM Player, export a
  game, preview it locally and deploy offline-capable browser versions.

## Building the engine

Only needed to change the engine itself. You need a C++20 compiler (MSVC, Clang
or GCC), CMake 3.21+, Ninja, Node.js 24, and vcpkg (Windows) or Homebrew (macOS)
for SDL3, FFmpeg and the other libraries in `vcpkg.json`. The JavaScript
workspace uses pnpm at the version `package.json` pins; `corepack enable pnpm`
provides it.

```bash
pnpm install
cmake --preset x64-release             # macos-debug on macOS, android-arm64 for Android
cmake --build --preset x64-release
ctest --test-dir out/build/x64-release -C Release
pnpm test
```

On Windows, build from a Visual Studio developer shell with `VCPKG_ROOT` set.
With a non-English MSVC, set `VSLANG=1033` so Ninja records header dependencies;
`ninja -C out/build/<preset> -t deps` shows what was recorded.

The build makes two Players. Games ship `PrismatiXPlayer`; `PrismatiXPlayerQA`
adds a local IPC server for automated QA and must never be released (a test
checks that the shipped Player contains none of it). Pack a QA build with
`--player <build>/PrismatiXPlayerQA.exe` and drive it with journey scripts:

```ts
// tests/journeys/choice.ts, run with `prismatix test`
export default async (px) => {
  await px.click("Start");
  await px.jump("chapter1:crossroads");
  await px.choose(0);
  await px.screenshot("after-choice.png");
};
```

```text
PrismatiXEngine/
├── PrismatiXEngine/   Player, Preview and Packager (Applications/), engine (Engine/), tests (Tests/)
├── Contracts/         JSON Schemas for every document the toolchain writes
├── packages/          authoring-sdk, cli, vscode-pxstory
├── runtime-sdk/       TypeScript types for runtime extensions
├── Samples/           Complete example projects
└── docs/              Guides, examples and the release contract
```

## License

PrismatiXEngine is licensed under the [MIT License](LICENSE). Release packages
carry the [third-party notices](THIRD_PARTY_NOTICES.md) and
[asset provenance](ASSET_PROVENANCE.json).
