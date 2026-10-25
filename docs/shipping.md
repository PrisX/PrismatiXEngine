# Shipping reference

Building, releasing and maintaining a game: distributions, keeping players'
saves working, updates and patches, user data, automated play tests,
importing an existing game, and the Android build.

- [Building a distribution](#building-a-distribution)
- [Packing for other platforms](#packing-for-other-platforms)
- [Keeping players' saves](#keeping-players-saves)
- [Comparing and patching distributions](#comparing-and-patching-distributions)
- [Signed PDX content overlays](#signed-pdx-content-overlays)
- [Save migration anchors](#save-migration-anchors)
- [Augmenting a published artifact](#augmenting-a-published-artifact)
- [Packaged Player user data](#packaged-player-user-data)
- [Testing with journeys](#testing-with-journeys)
- [Tool versions and engine checkouts](#tool-versions-and-engine-checkouts)
- [Importing stories from other formats](#importing-stories-from-other-formats)
- [Resolving assets while importing Story](#resolving-assets-while-importing-story)
- [Android (experimental)](#android-experimental)
- [Verifying a runtime bundle release](#verifying-a-runtime-bundle-release)
- [Web export](web-export.md)

## Building a distribution

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

For a release candidate, `release` validates, builds, packs and verifies every
archive in one step, then reminds you to commit `.prismatix-release/lineage.json`
so later releases keep loading players' saves. Put the settings every build of
the project should use in `prismatix.json` instead of repeating flags:

```json
"distribution": {"executableName": "first-light", "icon": "Assets/icon.png",
  "output": "release/First Light", "compression": "fast", "archiveShards": 4}
```

```bash
prismatix release my-game
```

Command-line options still win over these defaults. `"linkMedia": true`
hard-links the project's media into the build artifact instead of copying it
when both are on the same volume, so a multi-GiB game is not stored twice
(the artifact then shares those files with the project).

`"transcode"` makes distributions smaller: `build` and `pack` (not `dev`)
store PNG as WebP and Ogg Vorbis/WAV as Opus or Vorbis under the same paths,
which the Player recognises by content, so Story, UI and saves are unchanged.
Alpha is kept exactly; a file that would grow or fails to decode ships as
authored. Encodings are cached in `.prismatix/transcode-cache`, so only new or
edited media is encoded again, and `--no-transcode` packs the originals:

```json
"transcode": {"images": {"format": "webp", "quality": 90},
  "audio": {"format": "opus", "bitrate": 64},
  "exclude": ["Assets/Music/**"]}
```

`quality` is 1-100 or `"lossless"`, `effort` (0-6, default 4) trades encode
time for size, Opus `bitrate` is in kbit/s (default 96), and Vorbis takes
`quality` from -1 to 10 instead.

For media selected by a dynamic runtime path rather than a static Story/UI
reference, add `"resourceIncludes": ["Assets/MyFamily", "Assets/Audio/confirm.ogg"]`
to `prismatix.json`. Directories are expanded during validate/build; the files
are packaged as owned dependencies, while this author-only field is omitted
from the runtime project document. Missing or empty roots fail validation.
An extension command whose `resource` parameter has `"resourceBundle":
"directory"` (Live2D's `live2d.show model=...`) ships the folder of the file
Story names by itself, with no `resourceIncludes` entry
([studio-extensions.md](studio-extensions.md#files-a-resource-brings-along)).

### Executable name, icon and version information

`prismatix pack` names and brands the game's executable after `distribution`,
which is read only when packaging and never shipped as content:

```json
"distribution": {
  "executableName": "Hikari",
  "icon": "Assets/Branding/icon.png",
  "productName": "光之軌跡",
  "companyName": "Hikari Works",
  "copyright": "© 2026 Hikari Works",
  "version": "1.2.0"
}
```

- `executableName` replaces `PrismatiXPlayer`; the platform extension is kept
  (`Hikari.exe` on Windows).
- `icon` is a square PNG (at least 256x256; 1024x1024 for the sharpest macOS
  icon) or a Windows `.ico`. From a PNG, `pack` makes a Windows icon with 16,
  24, 32, 48, 64, 128 and 256 px frames, a macOS `.icns` and the Linux icon
  theme sizes. A PNG that is not square is centered on a transparent square,
  and `pack` warns when it is not square or smaller than 256 px. An `.ico` is
  used on Windows as authored; macOS and Linux use its largest frame.
- `productName` (default: the project name), `companyName`, `copyright` and
  `version` (default: the project's `version`) become the executable's
  version information, which Explorer shows in **Properties → Details** and
  Task Manager shows as the process description, and the macOS `Info.plist`.
  `version` may carry a pre-release suffix (`1.2.0-beta.1`): it is shown as
  written, and the numeric part (`1.2.0.0`) is the Windows file version and
  the macOS bundle version.

Every value is limited to 256 bytes on one line. Changing any of them, or the
icon file, rebuilds the package.

### Distribution layout

On Windows a distribution keeps only the executable at the top of its folder
and moves the runtime DLLs, notices and game data (`Content.pdx`, `Package/`)
into an engine folder, `Engine` unless `engineFolder` names another. This is
the default, `"layout": "bundled"`:

```text
Hikari.exe
Engine/
  Engine.manifest   SDL3.dll  avcodec-62.dll  ...
  Content.pdx  Package/  Notices/
```

`"layout": "flat"` keeps everything beside the executable instead, as a single
folder of files:

```json
"distribution": {"executableName": "Hikari", "layout": "flat"}
```

The engine folder is a Windows *private side-by-side assembly*:
`Engine.manifest` lists the DLLs, and the executable's application manifest
depends on an assembly of that name, so the Windows loader itself resolves
every DLL (the executable's imports, the DLLs' imports of each other, and
libraries loaded by name at run time) from that folder before the game's code
runs. It is still a single game process, so the taskbar, window icon, exit
code, command-line arguments and overlays such as Steam's behave exactly as in
the flat layout. The executable records the folder name, finds its game data
there and uses the same per-user save and log folders. `engineFolder` takes
1 to 64 letters, digits, `.`, `_` and `-` (it is also the assembly name) and
cannot be `Package`, `Save`, `Updates` or `Notices`. Rename the folder only by
repacking: the name is written into the executable. Besides the executable,
the top level holds only the CLI's `.prismatix-output.json` ownership marker,
which lets later packs replace the folder safely.

macOS and Linux have their own conventions and ignore `layout`:

- **macOS** packs a `<executableName>.app` bundle (the product name when
  there is no `executableName`): `Contents/MacOS/<executable>`,
  `Contents/Resources/` with the game data and `<executable>.icns`,
  `Contents/Frameworks/` for any runtime `.dylib`, and an `Info.plist` with
  `CFBundleName`, `CFBundleIdentifier`, `CFBundleShortVersionString`,
  `CFBundleVersion`, `CFBundleIconFile`, `LSMinimumSystemVersion` (the
  Player's deployment target) and `NSHighResolutionCapable`. The identifier
  comes from `distribution.macos.bundleIdentifier`, by default
  `com.example.<project id>` (an id that already contains a dot is used as
  is). Set your own reverse-DNS identifier before releasing: macOS keeps
  per-app permissions and preferences under it.

  ```json
  "distribution": {"executableName": "Hikari", "macos": {"bundleIdentifier": "jp.hikari-works.hikari"}}
  ```

  When `pack` runs on a Mac it relinks bundled libraries through
  `@rpath` (`install_name_tool`) and signs the bundle ad hoc
  (`codesign --sign -`), which Apple Silicon requires before anything runs;
  on Windows and Linux it signs with rcodesign when that is installed.
  An ad-hoc signature is enough for your own testing, not for players:
  Gatekeeper blocks downloaded apps that are not signed with a Developer ID
  and notarized. `pack` does both on any computer when it is given your
  certificate and API key
  ([Signing macOS apps from any computer](#signing-macos-apps-from-any-computer));
  on a Mac you can also do it yourself after `pack`, with your Apple
  Developer account:

  ```sh
  codesign --force --deep --options runtime --timestamp \
    --sign "Developer ID Application: Your Name (TEAMID)" "dist/Hikari/Hikari.app"
  ditto -c -k --keepParent "dist/Hikari/Hikari.app" Hikari.zip
  xcrun notarytool submit Hikari.zip --keychain-profile "notary" --wait
  xcrun stapler staple "dist/Hikari/Hikari.app"
  ```

  Re-sign and re-notarize after every repack.
- **Linux** keeps the executable (mode 755) and game data at the top, moves
  shared libraries into `lib/` (the Player searches `$ORIGIN/lib`), and adds
  `<executable>.desktop` and `icons/hicolor/<size>/apps/<executable>.png`
  (16 to 256 px). The `.desktop` entry uses the executable and icon theme
  names, so it works once an installer, a launcher or the player puts the
  executable on `PATH` and the `icons` tree under `~/.local/share/icons` or
  `/usr/share/icons`; the game itself runs from any folder without them.

`run --dev` and the packages that the Android and Web exports convert always
use the plain flat layout. Tools that take a distribution folder
(`verify-package`, `plan-patch`, `create-overlay`, `inspect-save --package`,
`pack --target web --web-package`) find the game data in any of these
layouts.

### Console output on Windows

The shipped Windows Player is a GUI program: double-clicking a game opens only
its window, never a console. Diagnostics always go to the log file in the
per-user data folder ([Packaged Player user data](#packaged-player-user-data)).
Started from a terminal (Command Prompt, PowerShell, Windows Terminal), the
Player attaches to that console and prints its log there too; the prompt
returns at once because Windows does not wait for GUI programs, so use
`start /wait Hikari.exe` in Command Prompt or
`Start-Process -Wait .\Hikari.exe` in PowerShell to wait for it. Pass
`--console` to open a console window of its own when there is no terminal, for
example from a shortcut:

```text
Hikari.exe --console
```

Output that is redirected to a file or pipe (scripts, CI, the CLI) is written
there unchanged. `PrismatiXPlayerQA`, the Packager and PreviewHost stay
console programs.

### Packing an artifact you already built

To distribute an artifact that has already been built, pass it to `pack`,
`build`, or `run` with `--artifact`. This skips author-source compilation and
the intermediate artifact copy; the native Packager still checks every input's
size and SHA-256 before accepting it. The artifact must be an intact CLI-owned
build directory with its `.prismatix-output.json` marker. Changes to Story or
UI source are **not** included until you build or otherwise update the artifact.

```bash
prismatix pack my-game --artifact my-game/.prismatix/artifact
```

If a post-build tool must add or replace artifact files, use
[`augment`](#augmenting-a-published-artifact) before `pack --artifact`;
ordinary media should instead be declared with `resourceIncludes`.

### Checks before packing

Before starting native packaging, the CLI checks free space on the output
volume against the artifact's indexed input sizes. With `--compression none`,
it stops early if the inputs already exceed the available space. Compressed
packages only get an advisory warning because the final archive size cannot
be known in advance. The check includes a small allowance for the Player and
archive metadata, but is an estimate, not a guarantee of a successful pack.
Use `--output` to select a volume with more space; `--ignore-disk-space` skips
this preflight if you have independently verified the package will fit.

Use `prismatix pack <project> --artifact <artifact> --check` to run that
native request and Story/UI/resource-contract preflight without writing a
distribution. Without `--artifact`, the CLI compiles a temporary artifact
first. The command reports a distinct `validated` result, never `packed`.
Media fingerprints intentionally deferred by normal Packager are also
deferred by `--check`; a successful preflight is not a verified PDX or a
guarantee that all media bytes will remain unchanged during packaging.
Add `--verify-media` to `pack --check` when you want SHA-256 verification of
every current artifact input without producing PDX. The `validated` event
reports `deferredMediaCount` (zero in this mode). Even full-input preflight
does not replace `verify-package` for a completed distribution.

### Large games

For large games, `pack`, `run`, and distribution `build` accept
`--archive-shards N` (1–64). The default `1` keeps `Content.pdx`; larger
values assign each resource to a stable URI-hash shard and emit
`Content-00.pdx`, `Content-01.pdx`, etc. The manifest lists only nonempty
shards, and Player mounts all listed files. Each compressed shard has its own
`Package/entry-cache-XX.json`. Keeping the same shard count preserves a
resource's shard when other resources are added; changing the count
repartitions the package and its cache. In an unencrypted package, unchanged
shards can be reused without rewriting their PDX. Encrypted packages also
reuse shards when the entire content identity is unchanged, but changing any
content rotates the current package-derived archive key and rebuilds all
encrypted shards. This is
split packaging, not automatic patch delivery: placing an extra `patch.pdx`
beside the game does not mount or apply it.

### Output folders and progress

Never choose Story, Assets, another source input or an unrelated existing folder as output. `pack` is an explicit distribution command; `run` packages and launches. Quote paths containing spaces.

Output replacement verifies the previous build's inventory. If you have played a distribution and it now contains saves/logs, or edited its files, use a new output directory; do not delete player data to bypass the guard. Test migration with a copy of the old saves before distributing an update.
When replacing an owned output at the same content/save version, Packager carries its former save fingerprint and any legacy key readers through CLI staging. A fresh, unrelated output directory has no previous release to inherit from; distribute it through a signed exact-base overlay or use an explicit versioned save migration when old saves must be accepted.

Long builds report loading, per-scene Story parse/compile counts across locales,
copy counts and bytes, fingerprinting, and publication on stderr. Packaging also forwards throttled native phase/count
updates as they happen. With `--json`, CLI progress is NDJSON on stderr and
native Packager events are streamed on stdout; neither waits for the whole
archive to finish before showing progress.

## Packing for other platforms

`pack` lays out a distribution for the computer it runs on unless `--platform`
names others. Any computer packs for any of `win32-x64`, `linux-x64`,
`darwin-x64` (Intel Macs) and `darwin-arm64` (Apple silicon):

```bash
prismatix add platform darwin-arm64 linux-x64     # once per project and CLI version
prismatix pack my-game --platform win32-x64,darwin-arm64,linux-x64
prismatix pack my-game --platform all --archive
```

`--platform` takes a platform, a comma list, `all` or `host`, and may repeat.
The project is compiled once; each platform then gets its own folder,
`<output>/<platform>` (by default `dist/<platform>`, beside `dist/web`), laid
out by that platform's rules ([Distribution layout](#distribution-layout))
with that platform's Player. Game content (the PDX archives, Runtime IR,
fonts and media) is the same for every platform. `--output` names the folder
that holds the platform folders and cannot be a packed distribution itself;
`distribution.output`, the folder of a plain `pack`, is not used.

`--archive` also writes the file players download beside each folder:
`<name>-<platform>.zip` for Windows and macOS, `<name>-<platform>.tar.gz` for
Linux, where `<name>` is `executableName` (else the project id). A Windows or
Linux archive holds one folder of that name; a macOS archive holds the `.app`.
Archives store Unix permissions, so the Player and libraries keep mode 755
whichever computer made them. A Windows file system does not keep the
executable bit, so a macOS or Linux distribution packed on Windows always gets
its archive, and the folder next to it is only for inspection: ship the
archive, and do not re-zip the folder with Explorer. Distributions contain no
symbolic links (runtime libraries are copied as files), so nothing else is
lost between file systems.

### Getting another platform's runtime

The CLI's npm package installs only this computer's native runtime
(`@prismatix/native-<platform>` carries `os` and `cpu` fields, so npm and pnpm
skip the others). `prismatix add platform <platform...>` adds the others to the
project:

1. It looks up `@prismatix/native-<platform>` at exactly the CLI's version in
   the npm registry (`PRISMATIX_NPM_REGISTRY`, else the `npm_config_registry`
   that npm and pnpm scripts set, else `https://registry.npmjs.org/`).
2. It downloads the package tarball and checks it against the SHA-512
   `integrity` the registry lists; a mismatch installs nothing.
3. It extracts it into `.prismatix/platforms/<platform>/` in the project (the
   starter's `.gitignore` already ignores `.prismatix/`), checks that its
   runtime manifest names that platform and the CLI's version, and records
   every file's size and SHA-256 in `platform.json`.

Nothing in the package runs: the host's own Packager copies that platform's
Player and libraries into the distribution. `pack` checks the recorded hashes
before using a runtime, and checks every requested platform before compiling,
so a missing one fails at once with the command that adds it (PXCLI1270).
After updating `@prismatix/cli`, run `add platform` again; a runtime of
another version is never used.

`prismatix add platform --check` lists which platforms the project can pack
for. Behind a proxy or private registry, download the tarball with npm's own
configuration and install that file:

```bash
npm pack @prismatix/native-darwin-arm64@0.1.0
prismatix add platform darwin-arm64 --from prismatix-native-darwin-arm64-0.1.0.tgz
```

`--from` also takes a runtime bundle as a `.tar.gz` or a folder (a release's
`prismatix-native-runtime-<version>-<platform>.tar.gz`, or a CMake install of
the `PrismatiXRuntime` component); it must still be that platform at exactly
the CLI's version. For engine development, `PRISMATIX_PLATFORM_RUNTIMES=<dir>`
uses `<dir>/<platform>/` runtime bundles directly, and a CLI running from an
engine checkout also finds `out/platforms/<platform>/`; both accept
pre-releases of the same line.

### What every host can produce

| | Packed on Windows | Packed on Linux | Packed on a Mac |
| --- | --- | --- | --- |
| Windows: icon, version information, manifest, `Engine/` | yes | yes | yes |
| macOS `.app`: Info.plist, `.icns`, signature | yes, signed with rcodesign | yes, signed with rcodesign | yes, `codesign` |
| Linux `lib/`, `.desktop`, icons, mode 755 | yes (in the archive) | yes | yes |
| Custom effect shaders | SPIR-V, DXIL, MSL | SPIR-V, DXIL, MSL | SPIR-V, MSL |

The Packager writes the Windows executable's resources itself, without
Windows' resource API, so a Windows build is the same from every host. It
rebuilds the executable's resource section, moves the relocation table behind
it and recomputes the PE checksum. Windows tools and Explorer read the result
exactly as before.

The macOS runtime's Player links only macOS libraries and `@rpath`, so its
`.app` needs no relinking; on a Mac `pack` still runs `install_name_tool` for
any bundled library, and elsewhere it reports a library that would need it.

Custom effects are compiled for every backend a platform's Player can use:
SPIR-V (Vulkan, Linux and Windows), DXIL (Direct3D 12, the Windows fallback)
and MSL (Metal). The DirectX Shader Compiler does not exist for macOS, so a Mac
cannot pack a Windows build of a project with custom effects: that pack fails
with PXPKG1511, and the Windows build is packed on Windows or Linux (a CI
runner, for example). Projects without custom effects pack for Windows on a
Mac.

### Signing macOS apps from any computer

Apple silicon runs no unsigned code, and macOS reports an app whose files
changed after signing as damaged. The macOS Player is signed ad hoc by its
linker, and `pack` signs the whole `.app`:

- On a Mac, ad hoc with `codesign --sign -` (Xcode Command Line Tools).
- Elsewhere, ad hoc with [rcodesign](https://github.com/indygreg/apple-platform-rs)
  (apple-codesign), when it is installed. The CLI never downloads it. Install
  it with `cargo install apple-codesign`, or put a release binary from
  `https://github.com/indygreg/apple-platform-rs/releases` on `PATH`, or name
  it with `PRISMATIX_RCODESIGN=/path/to/rcodesign`. Without rcodesign the
  `.app` is packed unsigned as a bundle and `pack` warns (PXCLI1227).

With a Developer ID Application certificate, `pack` signs with it and the
hardened runtime on any computer (rcodesign, also on a Mac), and with an App
Store Connect API key it notarizes the app and staples the ticket:

| Variable | Value |
| --- | --- |
| `PRISMATIX_MACOS_P12` | the certificate and private key exported as `.p12` |
| `PRISMATIX_MACOS_P12_PASSWORD` | its password (passed to rcodesign in a private file, never on the command line) |
| `PRISMATIX_MACOS_NOTARY_KEY` | the API key as JSON, made once with `rcodesign encode-app-store-connect-api-key -o key.json <issuer-id> <key-id> AuthKey_<key-id>.p8` |

Notarizing waits for Apple's answer, usually a few minutes. Keep these files
outside the project and pass them as secrets in CI. `--json` reports each
macOS distribution's `signature` (`developer-id`, `adhoc` or `none`) and
`notarized`.

What players see when they open a downloaded `.app`:

- **Developer ID and notarized**: it opens.
- **Ad hoc signed** (or Developer ID without notarization): macOS refuses it
  as from an unidentified developer. On macOS 15 and later the player opens
  it once from System Settings > Privacy & Security > Open Anyway; on earlier
  versions with right-click (Control-click) > Open. `xattr -dr
  com.apple.quarantine <Game>.app` clears the download quarantine as well.
- **Not signed as a bundle**: the same, and some versions call it damaged;
  clearing the quarantine still lets it run, because the Player keeps its
  linker signature. Sign it before giving it to anyone.

`scripts/check-desktop-layout.mjs --cross <platform>=<runtime>` is how CI packs
the Starter sample for Windows and Apple silicon from Linux and for Linux from
Windows, and checks each archive with that platform's rules. Only a Mac can
show Gatekeeper's verdict and run the `.app`; only Windows can run the `.exe`.

## Keeping players' saves

Every save records the fingerprint of the package that wrote it. When a new
package keeps the same `contentVersion` and `saveVersion` but its content
changed, the Player loads an older save only if the new package lists that
save's fingerprint as compatible. Otherwise it rejects the save with
`PXSAVE6111`. After each successful `pack`, `build`, or `run`, the CLI records
the fingerprint of the package it wrote, and the fingerprints that package
accepts, in `.prismatix-release/lineage.json` in the project. The next pack
passes that record to Packager, whatever `--output` it uses. **Commit this
file.** With it, packing to a new folder, packing on a clean CI machine, or
deleting an old `dist` folder no longer breaks existing saves. The file changes
only when the package fingerprint does.

When a pack finds no lineage file, no previous distribution in `--output`, and
no `--previous-package`, it warns with `PXCLI1222` that saves from an
already-shipped release with the same versions will be rejected. The warning
does not appear when a declared save migration leads into the current version.
For a first release, commit the lineage file that the pack writes.
`PXCLI1223` warns that the version changed since the recorded release but no
`saveMigrations` entry starts from the recorded version. A lineage file that
names another game ID stops the pack. See [save migrations](#save-migration-anchors).

To publish a standalone new version in a different directory while retaining
compatible saves from an earlier distribution, name that intact distribution:

```bash
prismatix pack my-game --artifact my-game/.prismatix/artifact --previous-package release/v1 --output release/v2
```

`--previous-package` also works with distribution `build` and `run`. The CLI
verifies the old package, checks its game ID and publisher key, and passes its
save identity and reusable archive data to Packager. The old directory stays
untouched; the new output must be absent or empty. Repacking into the same
output directory already carries this history without the option. Packager
combines the save identity it finds in the previous distribution with the
project's lineage file. Compatible
save identity does not make changed Story state compatible: advance the
content/save version and provide a migration when needed.

Story edits keep save identities through the `.pxids.json` sidecars; see
[keeping saves working while you edit](story-reference.md#keeping-saves-working-while-you-edit).

For an update, retain the previous packaged game and isolated test saves. Increment the intended content/save versions, provide the project's explicit save-migration chain, test dialogue moves and changed resource names/variable schemas, then launch the new packaged Player against copies of those saves. Never experiment on real player saves.

## Comparing and patching distributions

Each packaged distribution now includes `Package/content-index.json`, a
per-resource URI, SHA-256, source-size, and owning-archive inventory. To
compare two complete distributions, run:

```sh
prismatix plan-patch --base path/to/old-distribution --target path/to/new-distribution --json
```

This read-only plan reports added, modified, and removed resources, changed
raw bytes, and target archives containing additions or modifications. It
does not create a PDX, install an update, or establish trust in a package.
Use `prismatix verify-package path/to/distribution` to read and SHA-256-check
every resource in its PDX archives against the index, or add
`--verify-contents` to `plan-patch` to check both distributions before
comparing them. Native Packager also accepts `--verify-package` with an
absolute distribution path. These checks detect missing, corrupt, and
misindexed resources but are not cryptographic signatures: someone who can
replace the archives and their metadata together can forge a passing local
verification. The base and target must be packages for the same game and both
must include the new index.

For a small update between two complete, CLI-owned distributions, create a
whole-file delta bundle and explicitly apply it:

```sh
prismatix create-patch --base path/to/v1 --target path/to/v2 --output path/to/update-v1-v2
prismatix apply-patch --base path/to/v1 --patch path/to/update-v1-v2 --output path/to/updated-game
```

`create-patch` verifies both PDX contents and carries only changed files in
`Payload/`, plus `Patch/manifest.json` with the exact base and target file
inventories. `apply-patch` verifies the base and patch bundle, reconstructs a
complete target in a staging directory, verifies its PDX contents, and
publishes it after comparing every staged file against the target inventory.
Removed files are omitted from the assembled target. It refuses a wrong base
or modified/unowned output. For explicit replacement of the original game
directory after exiting the game, use:

```sh
prismatix apply-patch --base path/to/v1 --patch path/to/update-v1-v2 --in-place
```

Unchanged files are hard-linked into the staging directory when supported,
with copying as a fallback. A separate
`--output` copies the complete reconstructed distribution, so it needs space
for that full output. In-place replacement also needs room for changed files
and any fallback copies. Do not bypass an ownership refusal by removing saves
or other user files from the game directory.

The bundle is **not** a standalone `patch.pdx`, is not automatically loaded
at startup, and is not signed. It detects accidental corruption and wrong
versions, not a malicious distributor who can replace the bundle and its
metadata together.

Player accepts a signed, exact-base content update chain from `Package/updates`;
see [signed PDX content overlays](#signed-pdx-content-overlays).

## Signed PDX content overlays

`create-patch` / `apply-patch` still reconstruct and verify a complete new
distribution. The content-only PDX overlay is a smaller, exact-base update.
The Player scans bounded `.pdx` candidates in `Package/updates` and applies
the unique signed, exact-base update chain. Filenames do not control order.

### Publishing a signed update

```sh
prismatix overlay-keygen --private path/to/private-key.txt --public path/to/public-key.txt
prismatix pack game --artifact path/to/artifact --publisher-key path/to/public-key.txt --output path/to/v1
prismatix pack game --artifact path/to/updated-artifact --publisher-key path/to/public-key.txt --previous-package path/to/v1 --output path/to/v2
prismatix create-overlay --base path/to/v1 --target path/to/v2 --output path/to/patch.pdx --signing-key path/to/private-key.txt
prismatix verify-overlay --base path/to/v1 --patch path/to/patch.pdx --require-signature
```

For a content-only update, the complete v2 distribution is optional. A
published artifact can supply the target inventory directly:

```sh
prismatix create-overlay --base path/to/v1 --artifact path/to/updated-artifact --output path/to/patch.pdx --signing-key path/to/private-key.txt
prismatix verify-overlay --base path/to/v1 --patch path/to/patch.pdx --require-signature
```

To see the exact number of changed/removed resources and changed raw bytes
before allocating patch disk space, use the same artifact with `--check`:

```sh
prismatix create-overlay --base path/to/v1 --artifact path/to/updated-artifact --check --signing-key path/to/private-key.txt
```

`--check` verifies the current artifact inputs, the base distribution and the
publisher key, and writes nothing. It accepts `--updates-dir` and
`--optional-id` for a chained or optional patch. The raw bytes it reports are
not the final download size: changed resources of at least 64 KiB (except
streaming video) are compressed in the overlay.

The artifact must be a CLI-published directory with intact ownership
metadata: build it with `prismatix build --artifact-only`, or publish changes
with `prismatix augment`. Only changed resources enter the overlay; no second
complete distribution is written. A later v2 built from the same artifact with
`--previous-package v1`, the same Player, encryption setting, publisher key and
shard count has matching target documents.

To publish `patch2.pdx` without a complete v2, put the signed `patch.pdx`
in a directory of prior updates and use its verified target as the next
base. The new output must be outside the installed base and artifact:

```sh
prismatix create-overlay --base path/to/v1 --updates-dir path/to/prior-updates --artifact path/to/artifact-v3 --output path/to/patch2.pdx --signing-key path/to/private-key.txt
prismatix verify-overlay --base path/to/v1 --updates-dir path/to/prior-updates --patch path/to/patch2.pdx --require-signature
```

The same command works for later main updates: `--updates-dir` must contain
the complete, unique signed chain preceding the new patch. It rejects
damaged or unrelated main update candidates. Once verified, install the new
patch alongside the prior updates. To publish `adult.pdx` against the latest
main update, use the same `--updates-dir` and add `--optional-id adult`; it
remains independently enabled by the player. Updating `adult.pdx` after a
new main patch requires a new optional pack signed against that latest main
version. `verify-overlay --updates-dir` also works after the patch is installed:
it excludes the `--patch` file while resolving its preceding chain.

The two distributions must have the same game ID, Engine version, publisher
public key, and non-content runtime files, but distinct package fingerprints.
`--previous-package` makes v2 a standalone distribution that carries compatible
save identity and legacy key readers from v1. The CLI verifies v1 before
using it and leaves it untouched. Subsequent standalone versions should name
their immediate predecessor the same way. The project's committed
`.prismatix-release/lineage.json` carries the same save identity when the
previous distribution is not available (see
[same-version releases](#same-version-releases)). For incompatible Story state, advance
the content/save version and provide an explicit migration.
The public key is a lowercase, uncompressed SEC1 P-256 point in
`Package/manifest.json`. It also affects the package fingerprint. Keep the
private key outside the project and distribution, restrict access to it, and
back it up securely. `overlay-keygen` refuses to overwrite either output.

After verification, install the PDX in the distribution's `Package/updates/`
folder: `v1/Engine/Package/updates/patch.pdx` in the default Windows layout,
`v1/Package/updates/patch.pdx` in the flat layout and on Linux, and
`Contents/Resources/Package/updates/` of a macOS `.app`. Then restart Player. Place later patches in the same updates directory; Player
discovers their dependency order from
the signed base and target documents. The Player checks the pinned publisher key, signature, exact
base manifest and index bytes, target metadata, and every changed resource
before mounting it. It uses the base PDX archives and their original
decryption key, then mounts the overlay at higher VFS priority and applies
tombstones for removed resources. Route, Runtime IR, source map, content
version, and save migration settings come from the signed target manifest.
When content and save versions stay the same, Player also accepts a save from
an earlier fingerprint in the verified update chain, then checks its Runtime
document and exact execution anchor before restoring it. An in-place package
replacement also carries a bounded list of earlier same-version fingerprints
in `persistence.compatibleSaveFingerprints`; Player combines that list with
the signed main update chain. Legacy key readers from the installed base and
signed main target remain available across the chain, including when an
optional pack is enabled. An unrelated
same-version fingerprint is still rejected. If an update changes save or
Story state incompatibly, advance the content/save version and ship an
explicit save migration in the signed target.
Invalid or stale candidates are ignored. If two valid updates target the same
version, Player rejects the ambiguous chain and starts the base package.
It also retries the base package if Runtime initialization of an accepted
overlay fails. The installed update file is never modified by Player.
During chain discovery, Player reads each PDX's small manifest once and uses
its claimed base fingerprint to select candidates for the current version.
That claim is only a routing hint: selected candidates still undergo full
signature, exact-document, and changed-resource verification before mounting.

### Publishing an optional pack

Build a complete optional target distribution from the latest main version.
Keep its game ID, Engine version, publisher key, content version, save version,
and persistence identity the same. The optional target can add or replace
resources and routes. For example, after v2 is the latest main version:

```sh
prismatix create-overlay --base path/to/v2 --target path/to/v2-with-adult --output path/to/adult.pdx --signing-key path/to/private-key.txt --optional-id adult
```

Install `adult.pdx` beside the main updates in `Package/updates`. It remains
off by default. From the installed distribution, run
`PrismatiXPlayer --optional-pack adult on` or
`PrismatiXPlayer --optional-pack adult off`, then restart Player. The choice
is atomically stored in that game's per-user data directory as
`optional-pack.txt`, so
reinstalling the game does not reset it. Only one optional pack can be enabled
at a time. The enabled pack must be signed for the final main update version;
an older optional pack is ignored after a new main update until the publisher
ships a compatible replacement. The Player uses the main update fingerprint
for save identity while the optional pack is enabled. Saves inside content
that is absent when the pack is off may still need a migration or the pack to
be enabled again.

### Format and verification

An overlay is an unencrypted PDX5 file whose virtual resources are exactly:

- `__prismatix_overlay__/manifest.json` (at most 16 KiB)
- `__prismatix_overlay__/target-manifest.json` (at most 4 MiB)
- `__prismatix_overlay__/target-content-index.json` (at most 128 MiB)
- `__prismatix_overlay__/signature.bin` (exactly 64 bytes, present when the overlay is signed)
- every added or changed target resource, by its original virtual URI

The overlay manifest has `format: "PrismatiXPackageOverlay"`, a
`schemaRevision` of 1. An optional pack adds `kind: "optional"` and a bounded
lowercase `optionalId`. Every overlay contains `gameId`,
`basePackageFingerprint`, `targetPackageFingerprint`, and SHA-256
fields named `baseManifestSha256`, `baseContentIndexSha256`,
`targetManifestSha256`, and `targetContentIndexSha256`. The two target
documents copy the Packager-produced `Package/manifest.json` and
`Package/content-index.json`. The reserved overlay prefix cannot be an
ordinary package resource.

A signed overlay signs the SHA-256 digest of the exact overlay manifest bytes,
prefixed with the ASCII bytes `PrismatiXPackageOverlaySignature` and a NUL byte.
`signature.bin` contains a raw P-256 ECDSA SHA-256 signature in
`r || s` form. The signed manifest hashes the exact base and target
documents; the target index hashes every changed payload. Verification
rejects missing, extra, mismatched, or damaged entries. Unchanged resources
remain in the base PDX even if the target changes their shard assignment.

`prismatix verify-overlay` verifies signatures whenever present. Add
`--require-signature` for the Player trust policy; it rejects an unsigned
overlay. Unsigned overlays remain available for explicit
inspection and are never loaded automatically.

The distribution-to-distribution form of `create-overlay` fully verifies both
input distributions, rejects changed
Player/runtime files, streams changed content directly from target archives
into an uncompressed PDX5,
re-verifies its output, and publishes without replacing an existing file.
It rechecks manifest and index bytes before publication. The staged PDX needs
space for the final patch, but changed resources no longer need separate
extracted files. Each streamed entry is checked against the target index size
and SHA-256 while writing. Unencrypted compressed PDX entries decode
with a capped zstd history window while streaming, and complete size/CRC
verification at end of stream. Legacy whole-entry encrypted and compressed
entries still buffer the decoded entry; encrypted chunked entries stream.
On Windows, publication moves the verified PDX within the output volume without
the replace-existing flag, so it also works on volumes without hard-link
support. A competing output created during the build is preserved. Other
platforms currently publish with a no-replace hard link.

### Current limits

Only one optional pack can be enabled at a time, and switching requires a
Player restart. There is no in-game toggle UI yet. A signed update chain may
contain at most 64 regular PDX candidates in a
dedicated directory, and ambiguous branches fail closed. Publishing from a
prior signed chain still rechecks all artifact inputs and verifies the complete
installed base.

A local process that can rewrite distribution files while the game starts can
still race verification and mounting; protect the base distribution and its
Player with the normal installation boundary.

## Save migration anchors

### Same-version releases

A release that keeps `contentVersion` and `saveVersion` needs no migration,
but its package fingerprint still changes with its content. The Player accepts
a save from an earlier fingerprint only when the current package lists that
fingerprint in `persistence.compatibleSaveFingerprints`. Packager builds the
list from the distribution it replaces and from the project's
`.prismatix-release/lineage.json`, which the CLI updates after each pack.
Commit that file with each release. It records the latest package fingerprint,
its content/save version, the earlier same-version fingerprints, and the legacy
persistence readers. These are the values the shipped `Package/manifest.json`
already contains in plain text. Up to 64 same-version fingerprints are kept; Packager fails
with `PXPKG1428` beyond that, which means it is time to advance the version.
When the version changes, the list starts over and the declared migrations
decide which old saves still load.

The Player reports these load failures separately:

| Code | Meaning |
| --- | --- |
| `PXSAVE6115` | The save belongs to a different game ID. |
| `PXSAVE6111` | Same content/save version, but the save's package fingerprint is not current or listed as compatible. The package was probably built without the previous release's lineage. |

To recover from `PXSAVE6111` after a release has shipped, restore the lineage
file from version control, or pack again with `--previous-package` naming the
shipped distribution. The new package then accepts both fingerprints.

### Anchors

When a release changes `contentVersion` or `saveVersion`, declare each migration
step in `prismatix.json` and provide its `.pxsave-migration` document. A step
can also rename variables or routes and change the Runtime IR path. Test it
against copies of saves from the released Player before shipping.

The `anchor` field controls the saved execution position:

| Policy | Behavior |
| --- | --- |
| `preserve` | Keep the saved document, source, and operation IDs. |
| `map` | Require an exact mapping for the saved anchor; reject an unlisted anchor. |
| `map-or-preserve` | Map listed anchors and keep an unlisted anchor's IDs. |

Use `map-or-preserve` when a large Story keeps most identities but moves a few
operations. It avoids listing every unchanged anchor in a file limited to
100,000 mappings and 4 MiB by the native migration reader. For example:

```json
{
  "format": "PrismatiXSaveMigration",
  "schemaRevision": 1,
  "id": "chapter-1-to-2",
  "from": {"contentVersion": "chapter-1", "saveVersion": 1},
  "to": {"contentVersion": "chapter-2", "saveVersion": 2},
  "anchor": {
    "policy": "map-or-preserve",
    "mappings": [{
      "from": {"runtimeDocumentId": "old-document", "sourceId": "old-line", "operationId": "old-op"},
      "to": {"runtimeDocumentId": "new-document", "sourceId": "new-line", "operationId": "new-op"}
    }]
  },
  "operations": []
}
```

`prismatix ids --sync` helps review added and removed Story identities but
does not generate or verify a migration. The Authoring SDK can compare compiled
Runtime IR from two releases and require a replacement for each old anchor
that has disappeared:

```ts
import {planSaveAnchorPolicy} from "@prismatix/authoring-sdk";

const plan = planSaveAnchorPolicy(previousRuntimeIr, nextRuntimeIr, [
  {fromOperationId: "old-op", toOperationId: "new-op"},
]);
if (!plan.valid) throw new Error(JSON.stringify(plan.diagnostics));
const migrationAnchor = plan.value!.anchor;
```

Use `migrationAnchor` as the document's `anchor` field. The inputs are the
canonical Runtime IR documents; operation IDs identify commands within each
document. The helper retains exact old anchors, maps explicitly reviewed
replacements, and reports missing or duplicate identities. Compare every
supported locale's Runtime IR and test real old saves: the helper cannot tell
whether a surviving ID or replacement still means the same thing. The Player
checks that the resulting document and exact source/operation pair exist
before restoring a slot. It rejects an unchanged anchor that was removed.
Existence alone cannot prove the operation has the same narrative meaning or
that variables, resources, and routes still make sense. Validate representative
old slots at each changed position in the packaged Player.

A content overlay cannot replace the Player executable.

## Augmenting a published artifact

Prefer declaring ordinary runtime files with `resourceIncludes` in the source
project. Use `prismatix augment` only when a post-build tool genuinely creates
or replaces files after the canonical artifact build. Never edit an artifact
or `.prismatix-output.json` directly.

Create a manifest outside the artifact, for example `artifact-augment.json`:

```json
{
  "format": "PrismatiXArtifactAugmentation",
  "schemaRevision": 1,
  "owner": "my-importer",
  "files": [
    {"source": "Generated/bonus.png", "target": "Assets/bonus.png", "mode": "add"}
  ]
}
```

`source` is relative to the author project; `target` is relative to the
published artifact. Both must be safe paths to regular files, without symlinks.
An `add` target must not exist. To replace an owned artifact file, use
`"mode":"replace"` and supply its current 64-character SHA-256 from
`.prismatix-output.json` as `expectedFingerprint`. A replacement fails if a
newer build changed that file. A different augmentation owner cannot replace
another owner's file. The CLI also rejects duplicate targets and its reserved
ownership files.

```bash
prismatix build my-game --artifact-only --output my-game/.prismatix/artifact
prismatix augment my-game --artifact my-game/.prismatix/artifact --manifest my-game/artifact-augment.json
prismatix pack my-game --artifact my-game/.prismatix/artifact
```

The operation checks the old artifact's complete inventory, takes the same
publication lock as `build`, stages unchanged files using hard links when the
filesystem allows, and publishes a new complete ownership inventory atomically.
It writes `.prismatix-augmentations.json` into the artifact to track target
owners. Failure keeps the old artifact; no partially augmented output is
published. Rebuilding the same artifact path is allowed afterward, but a fresh
build replaces the overlay, so rerun `augment` if it is still needed. Because
ownership verification and fingerprinting read the complete artifact, this is
not a block-level incremental build or a way to avoid hashing large media.

`augment` validates publication safety, not whether a replacement UI scene or
project document is semantically correct. `pack --artifact` performs the
native package validation, including file hashes, canonical routes and
contracts. Test the resulting Player before shipping.

## Packaged Player user data

The packaged Player treats its installation directory as game content, not as
a writable save directory. It keeps `Save/config.dat`, `Save/profile.dat`,
`Save/save_<slot>.pxsav`, `Save/autosave.pxsav`, and `logs/PrismatiXPlayer.log`
under a per-user directory chosen by SDL's `SDL_GetPrefPath`. The organization
is `PrismatiXEngine`; the application directory is `Game ` followed by the
SHA-256 of the package's stable `gameId`. It does not change when content is
repacked or the installation is moved.

To choose another parent directory, set `PRISMATIX_PLAYER_DATA_ROOT` to an
**absolute** path before starting Player. Player still creates a separate
`Game <SHA-256(gameId)>` child within that parent. This is useful for isolated
tests or portable deployments; put it outside an artifact/distribution output
if you want the ownership-safe CLI to update that output in place. An invalid
or unwritable data location fails startup rather than silently falling back to
the package or launcher working directory. Developer launches without an
adjacent package keep their existing relative `Save/` and `logs/` behavior.

On the first launch with this storage policy, Player copies recognized files
from the older `<package>/Save/` directory to the new location. It recognizes
settings, profile, numeric slots, autosave, and their `.legacy*.bak` key
migration backups. Existing destination files win; nothing in the package is
overwritten or deleted. A marker in the new user-data directory prevents a
later launch from restoring stale old saves after a player deletes or changes
them. A failed or unsafe import stops startup and retains the old files for
recovery. Persistence file path strings are interpreted as UTF-8, including
Windows paths with non-ASCII user names.

The old portable `Save/` and `logs/` directories remain beside an installation
that was already run with an older Player. Their bytes are preserved, so an
in-place `prismatix pack` may still reject that *old* distribution as modified.
After verifying the imported saves, move those legacy directories to a backup
outside the distribution before repacking. New packages no longer create them
by default. `PrismatiXPlayer --inspect-save <path> --package <Package/manifest.json>`
continues to accept an explicit save path.

## Testing with journeys

`prismatix test` plays a packaged game with scripts and fails when one
throws, so a build can be checked screen by screen without clicking through
it. Pack with the QA Player, which answers the test interface (the shipped
Player has none of it):

```bash
prismatix pack . --output dist/qa --player <runtime>/PrismatiXPlayerQA.exe
prismatix test --package dist/qa
```

Journeys are `tests/journeys/*.ts` (or paths given on the command line).
Each gets a fresh Player with its own user data:

```ts
import type {PrismatixTestClient} from "@prismatix/cli/test";

// The default limit is 180 s; a long tour sets its own (--timeout overrides).
export const timeoutMs = 600_000;

export default async function tour(px: PrismatixTestClient) {
  await px.waitFor({appState: "title", idle: true});
  await px.jump("chapter2:after_school");
  // Click through lines until one matches, in one request.
  await px.advanceUntil({textContains: "放學後"});
  await px.screenshot("after-school.png");
  await px.advanceUntil({choices: true});
  await px.choose(0);
  await px.save(1);  // the thumbnail is the next Stage frame, without the HUD
}
```

- `px.advanceUntil(condition, {timeoutMs, maxLines})` takes the same
  conditions as `px.waitFor`. The Player clicks a few lines (and timed waits)
  per frame and checks the condition after each, so it never clicks past the
  line it looks for. It fails at a choice the condition does not ask for,
  when the story ends, and after `maxLines` (5000) lines; it returns the
  state with `advancedLines`.
- `px.save(slot)` keeps the state at the request and writes it once the next
  Stage frame is drawn, as the Save screen does, so the slot shows a real
  thumbnail. Inside `px.batch([...])`, which runs in one frame, `save` writes
  at once and without a thumbnail.
- `px.tap(x, y)` clicks at a point in game pixels; `px.tap(x, y, {button:
  "secondary"})` right-clicks. While the Player boots, `state.splash` names
  the startup screen on show (`{index, scene}`).
- Screenshots with relative paths go to `.prismatix/test-results/<journey>`.
  On failure the report shows the tail of the Player log and keeps the user
  data.


## Tool versions and engine checkouts

The global command delegates project commands to the exact local CLI pinned in the game's package.json. Commit that manifest and its lock. A missing or mismatched local CLI is diagnosed instead of silently using the global version. After preparation, dev/build use local packages and bundled runtime offline. Upgrade tool versions deliberately in a branch, reinstall the pinned dependencies, and test old saves and the packaged Player before distributing an update.

If you intentionally link a local Engine checkout with `file:` dependencies
(`link:` in a pnpm project), run `pnpm run build` in that checkout after editing
CLI or Authoring SDK source. A missing or stale pinned CLI names the restore
command for the project's lock: `pnpm install --frozen-lockfile` or `npm ci`.
Local source packages carry a content-based build stamp; an out-of-date `dist`
now fails with `PXSDKBUILD1001` instead of silently running old compiler code.
Registry packages contain no source tree and do not perform this local check.

Engine contributors can pass a local build directory as `--runtime
out/build/x64-release/PrismatiXEngine`; a CLI linked from an engine checkout
finds that checkout's build by itself (`PRISMATIX_NO_CHECKOUT_RUNTIME=1` turns
this off), and `create` run from a checkout links the new project to it with
`file:` dependencies, so it works offline.

## Importing stories from other formats

### Artemis games

`prismatix import artemis <extracted-game> [project]` converts an extracted
Artemis game: scenes, save-ID sidecars and the story index, characters and
layered portraits, the gallery (`Content/game.pxgame`), the `characters` and
`storyAssets` fields of `prismatix.json`, and `docs/conversion-report.json`.
Defaults such as fade times, screen size and font sizes come from the game's
own `list_windows.tbl`. Anything that only this game does (act presets,
scrambled movie names, replay scenes, chapter rules, key bindings, asset roots)
goes in `artemis-import.json` beside `prismatix.json`, which the command
validates.

Re-running the import keeps the save IDs already in the sidecars, so saves made
with an earlier import still load. The output uses the short Story forms; pass
`--long-forms` to spell every argument by name. The importer does not copy the
original media into the project.

### Writing your own importer

The Authoring SDK provides the output side of a story importer. A format
adapter decodes its own source files and translates their commands into
PrismatiX Story lines. The SDK checks those lines, assigns the adapter's
explicit execution IDs to an adjacent `.pxids.json` sidecar, and assembles a
multi-scene, multi-locale `story.pxindex`. It does not decode KAG, SCN, PSB, or
another game's proprietary format by itself.

```ts
import {assembleImportedStoryProject} from "@prismatix/authoring-sdk";

const imported = assembleImportedStoryProject({
  id: "example-story",
  entryScene: "intro",
  locales: ["en"],
  chapters: [{id: "main", title: "Main"}],
  scenes: [{
    id: "intro", chapter: "main", requiredLabels: ["start"],
    sources: {en: {
      path: "Story/en/intro.pxstory",
      lines: [
        {source: "*start", id: "original.intro.start"},
        {source: "Hello.", id: "original.intro.greeting"},
        {source: "[end]", id: "original.intro.end"},
      ],
    }},
  }],
});
if (!imported.valid) throw new Error(JSON.stringify(imported.diagnostics));
// Publish imported.value.files together, then compile the project.
```

An adapter can use `serializeStoryCommand` for named JSON arguments and
`createImportedResourceResolver` for unambiguous media lookup. It should
decode one source scene at a time rather than retain all raw source data. The
assembler takes the translated scenes and returns canonical index, Story,
and sidecar text without writing files. Write the returned files as one
reviewable set; a failed import returns no set to publish.

Every executable line must have a stable ID assigned from the original
source. Blank lines, comments, speaker declarations, and `choice.wait` do not
take IDs. The SDK rejects duplicate IDs within a shared identity namespace,
different ID or operation-kind sequences across locales, missing locales,
unsafe or colliding paths, and source or sidecar files above 16 MiB. A later
`compileStoryProject` or `compileProject` must still check command meaning,
resources, labels, and cross-scene jumps. The assembler does not infer how
two versions of a story should map old saves; use the [save migration
tools](#save-migration-anchors) for that review.

For an existing importer whose labels previously received generated
`sourceId` values, `buildImportedStorySource(path, lines,
{legacyLabelSourceIds: {documentId}})` can recreate that ID seed while still
requiring explicit IDs on dialogue and commands. Use the exact `documentId`
from the old import. When a sidecar already exists, reconcile the new source
against it with `synchronizeAlignedStoryIdentityMap` or
`synchronizeStoryIdentityMap` before publishing: prior edits may have preserved
label IDs that a fresh generation would change. New importers should assign
explicit IDs to labels too.

A decoder for another engine's script format and the translation of its
commands stay in the importing project. Such an adapter can call
`buildImportedStorySource` per scene and keep its existing sidecar
reconciliation. The SDK provides a general index assembler and an
ambiguity-reporting media resolver; moving an older importer onto them is
project migration work, because its existing edited sidecars must remain
stable and its media fallback may choose among source assets with
game-specific rules. The SDK does not need those rules to support other
importers, and it is not a general KAG or PSB input decoder.

## Resolving assets while importing Story

An importer can enumerate media once and use the Authoring SDK to match names
from an external script format to project-relative resource paths. Pass only
paths that belong to the resource category being imported:

```ts
import {createImportedResourceResolver} from "@prismatix/authoring-sdk";

const indexed = createImportedResourceResolver([
  "Assets/Characters/hero.png",
  "Assets/UI/hero.png",
  "Assets/BG/school.png",
]);
if (!indexed.valid || !indexed.value)
  throw new Error(JSON.stringify(indexed.diagnostics));

const image = indexed.value.resolve("hero.png");
if (image.kind === "ambiguous")
  throw new Error(`Choose an exact path: ${image.candidates.join(", ")}`);
if (image.kind !== "found") throw new Error(`Missing or unsafe image: ${image.query}`);
// Write image.path into a Story resource argument.
```

Exact project-relative paths win. A bare name is case-insensitive and matches
the filename stem; if the name includes an extension, that extension must also
match. Multiple candidates produce `ambiguous` with deterministic, sorted
paths. The resolver never selects one by file size, directory order, or locale.
It rejects unsafe index paths, and a query with a directory never falls back to
another directory's basename. If two source paths differ only by case, even an
exact lookup is ambiguous: this avoids a project that works on one filesystem
and opens a different asset on another.

The input is an array of **already enumerated** project-relative paths. The
resolver does not walk folders, decode external formats, create asset UUIDs, or
register assets in the package. Continue to use Story static resource
references, `resourceIncludes`, or the project asset catalog for build-time
dependencies. Keep the source format decoder in its importer and report every
missing or ambiguous lookup before writing generated Story files.

To write a command after resolving its resources, use
`serializeStoryCommand(name, arguments)`. It quotes strings and encodes JSON
values so the Story parser can read them back:

```ts
import {serializeStoryCommand} from "@prismatix/authoring-sdk";

const line = serializeStoryCommand("show", {
  character: "hero", asset: "Assets/Characters/hero smile.png",
  transition: "auto",
});
if (!line.valid || !line.value)
  throw new Error(JSON.stringify(line.diagnostics));
// line.value can be appended to a .pxstory file.
```

This helper checks that the line is syntactically parseable; project
compilation still checks whether the command, resource, character, and flow
targets are valid. `[if ...]` uses an expression grammar and must be authored
separately. To keep save identities stable across reimports, follow the Story
sidecar workflow in the Authoring SDK README; neither helper writes files or
silently updates a published identity map.

## Android (experimental)

> **Status: builds, not yet run on a device.** The arm64 Player library,
> the Gradle project and `prismatix pack --target android` have produced
> debug and signed release APKs and an AAB from the `minimal` starter on a
> Windows host. Nothing has been installed on a phone or emulator yet, so
> runtime behaviour on Android (rendering, audio, lifecycle, touch,
> text-to-speech) is unverified. Please update this page after the first
> device run.

This page covers phases A0–A2 of the Android plan in the
2026-09-30 release review.

### What is verified

| Area | State | Where |
| --- | --- | --- |
| arm64-android vcpkg dependencies | All 27 target ports build from a Windows host (NDK r27d) | `vcpkg/triplets/arm64-android-prismatix.cmake` |
| `libmain.so` | Links; stripped 19.6 MB; needs only FFmpeg and system libraries; 16 KB pages | `android-arm64` preset |
| Gradle project | `assembleDebug`, `assembleRelease`, `bundleRelease` succeed | `packages/cli/templates/android` |
| `prismatix pack --target android` | Builds the APK (and AAB when signing is configured) | [below](#build-an-apk) |
| Reading the package from the APK | Tested on desktop through SDL only | `Engine/IO/FileSource.h` |
| Background, foreground, low memory, terminate | Rules unit-tested on Windows only | `Engine/Platform/AppLifecycle.h`, `Engine/Runtime.cpp` |
| Touch gestures and `platform.touch` | Unit-tested with synthetic finger events | `Engine/Platform/TouchGestures.h`, [Touch input](#touch-input) |
| Text-to-speech (self-voicing) | Compiles; JNI to `android.speech.tts.TextToSpeech` | `Engine/Accessibility/SpeechService.cpp`, `PrismatiXSpeech.java` |
| TalkBack (screen reader tree) | Not supported | `Engine/Accessibility` |
| Native extensions (`android-arm64`) | Loaded, verified and called on the Android 15 x86_64 emulator through ARM translation | [native-extensions.md](native-extensions.md#android) |
| Play Asset Delivery | Not started | phase A3 |

### Prerequisites (host)

- Android SDK with `platforms;android-35` and `build-tools;35.0.0`, and
  `ANDROID_HOME` pointing at it. The template turns off AGP's automatic
  SDK downloads, so a missing package fails the build by name.
- Android NDK r27 or newer; set `ANDROID_NDK_HOME`.
- JDK 17 or newer in `JAVA_HOME`, and Gradle 8.9 or newer (tested with
  JDK 21 and Gradle 8.11.1). The first Gradle run downloads the Android
  Gradle Plugin from Google's Maven repository into `GRADLE_USER_HOME`.
- vcpkg (`VCPKG_ROOT`; Visual Studio's bundled copy works) and the desktop
  build of this repository: the Android build runs the host `shadercross`
  to compile built-in shaders.

### Build `libmain.so`

```sh
cmake --preset android-arm64
cmake --build --preset android-arm64
```

The preset chain-loads the NDK toolchain into vcpkg with the overlay
triplet `arm64-android-prismatix`: API 26, release-only, everything static
except FFmpeg, which is LGPL and ships as replaceable shared libraries.
The preset assumes a Windows host (`VCPKG_HOST_TRIPLET=x64-windows` and the
`shadercross.exe` path); on macOS or Linux, override both. To keep the
large Android trees off the system drive, add
`-B <dir> -DVCPKG_INSTALLED_DIR=<dir>/vcpkg_installed`.

Each Player directory is self-contained:

- `android/shipped/arm64-v8a/`: stripped `libmain.so`, the FFmpeg
  `lib*.so` files and `prismatix-player.json`. Pass this folder to the CLI.
- `android/shipped/symbols/arm64-v8a/libmain.so`: unstripped, for
  `ndk-stack`.
- `android/qa/...`: the QA Player with the QA IPC server. The CLI
  refuses to ship it.

Notes from the first build:

- NDK r27's libc++ 18 needs `-fexperimental-library` for `std::jthread`
  and `std::stop_token`, and has no floating-point `std::from_chars`
  (the engine uses `px::ParseFiniteDouble`).
- vcpkg's `harfbuzz.pc` asks for `-lc++` (the shared C++ runtime); the
  build drops it because `libc++` is linked statically.
- Configure fails if the template's SDL Java glue
  (`app/src/main/java/org/libsdl/app`, copied from the SDL source release)
  differs from the native SDL version. When vcpkg updates SDL, copy the
  matching release's `android-project/app/src/main/java/org/` over it.

### Build an APK

Add the Android settings to `prismatix.json`:

```json
"distribution": {
  "android": {
    "applicationId": "com.example.firstlight",
    "versionCode": 3,
    "versionName": "1.2.0",
    "label": "First Light",
    "icon": "Assets/Branding/icon-512.png",
    "keystore": {
      "path": "../keys/upload.jks",
      "alias": "upload",
      "storePasswordEnv": "FIRSTLIGHT_STORE_PASSWORD",
      "keyPasswordEnv": "FIRSTLIGHT_KEY_PASSWORD"
    }
  }
}
```

Only `applicationId` is required. `versionName` defaults to the project
`version`, `label` to the project name, and the orientation follows the
resolution (landscape unless taller than wide). `icon` is a square PNG
(48–4096 px). The keystore names environment variables; passwords never go
in the file, and a `*pass*` key in `keystore` is rejected.

```sh
prismatix pack my-game --target android \
  --android-player out/build/android-arm64/android/shipped/arm64-v8a \
  [--artifact <built artifact>] [--gradle <gradle or its install dir>] \
  [--android-sdk <dir>] [--output <dir>] [--debug]
```

- Without a keystore (or with `--debug`) the result is a debug-signed
  `dist/<id>-android/<id>-debug.apk` for testing.
- With a keystore it is a signed `<id>.apk` plus `<id>.aab` for Play.
- `<output>/build/package` holds the packed game (reused when unchanged) and
  `<output>/build/gradle` the Gradle project (Gradle's caches survive
  between runs). The game goes into `assets/` stored uncompressed, laid out
  as on desktop (`Package/…`, `Content*.pdx`, `Notices/`); the desktop
  Player and its libraries are left out.

To do the same by hand, copy the template, put the `.so` files in
`app/src/main/jniLibs/arm64-v8a/`, the packed game in
`app/src/main/assets/`, optionally write `prismatix.properties` (keys:
`applicationId`, `versionCode`, `versionName`, `screenOrientation`,
`keystore`, `keyAlias`, `storePasswordEnv`, `keyPasswordEnv`), and run
`gradle assembleDebug`.

Install and watch the log on a device:

```sh
adb install -r dist/<id>-android/<id>-debug.apk
adb logcat -s SDL SDL/APP PrismatiX
```

#### How the Player finds the game

`io::PackageFileSource()` is `SdlFileSource()` on Android. The Player opens
`Package/manifest.json`, every `*.pdx` shard and any VFS directory mount
through `SDL_IOFromFile`, which tries the app's internal storage first and
then the APK's `assets/`. Files that a later download places at the same
relative path in internal storage would be picked up first; nothing uses
this yet. Saves, settings, the profile and logs use `SDL_GetPrefPath`.
Signed single-file updates (`Package/updates`) are scanned with
`std::filesystem`, so they do not apply on Android yet. Keep APK assets
under about 1–2 GiB for side-loading; larger games need Play Asset
Delivery (phase A3).

#### Lifecycle

The Runtime registers an SDL event watch. These rules are unit-tested on
Windows; they have not run on a phone.

- **Leaving the foreground:** the Player saves first, then all media pauses.
  The save goes to the autosave slot if a game is in progress, followed by
  the settings and profile, and the Player waits for the writes. Nothing is
  rendered until the app returns.
- **Returning:** only the audio tracks and videos the lifecycle paused
  resume. The frame clock restarts, and text density is recomputed for the
  new surface size.
- **Low memory:** drops textures not drawn in the last frame, font files no
  open font uses, and unreferenced decoded audio.
- **Texture budget:** resident textures are trimmed, least recently drawn
  first, to 192 MiB on Android and iOS (512 MiB on desktop); a texture drawn
  in the last frame always stays. `"player.textureBudgetMiB": {"mobile": 256}`
  in the project settings changes it (32-16384, `desktop` likewise).
  `distribution.transcode` shrinks the APK's media but not this figure:
  textures are decoded to RGBA either way.
- **Terminating:** saves again only if the game ran since the last
  background save, then quits.
- `SDL_EVENT_RENDER_TARGETS_RESET` and `SDL_EVENT_RENDER_DEVICE_RESET`
  recreate the renderer's offscreen targets. A device reset also reloads
  file textures.

### Touch input

Fingers are turned into gestures. Each gesture fires the same command the
mouse, keyboard or gamepad already fires:

| Gesture | Same as | Default effect |
| --- | --- | --- |
| Tap | Left click | Advance the text, or activate the control under the finger |
| Long press (0.5 s) | `toggle-ui` (H) | Hide or show the message window |
| Two-finger tap | Right click | Close the open menu (back one screen); in the game, whatever right click does |
| Swipe up | `backlog` (B, wheel up) | Open the backlog |
| Drag | Held left button | Sliders and other draggable controls |

The Android Back button is a right click too.

A tap acts when the finger lifts, so a swipe or long press never advances
the text as well. SDL's synthetic mouse copies of touches are ignored.
Mouse, keyboard and gamepad input is unchanged.

`platform.touch` is Player UI data (boolean), so TSX can show
touch-friendly variants:

```tsx
<Group name="TouchToolbar" visibleWhen="platform.touch" ... />
<Group name="HoverDock" visibleWhen="!platform.touch" ... />
```

The project setting `player.touchProfile` controls both:

| Value | Gestures | `platform.touch` |
| --- | --- | --- |
| `"auto"` (default) | on | `true` while the last pointer input was a finger |
| `"on"` | on | always `true`: preview touch layouts on desktop |
| `"off"` | off; the first finger acts as a plain mouse pointer | always `false` |

### Detecting the platform

The device the game runs on is fixed Player UI data:

| Key | Value |
| --- | --- |
| `platform.os` | `"windows"`, `"macos"`, `"linux"`, `"android"` or `"ios"` |
| `platform.mobile` | `true` on Android and iOS |
| `platform.desktop` | `true` everywhere else |

On mobile the game is always fullscreen: the Player ignores
`set.fullscreen.off`, so dim or disable a window-mode choice there:

```tsx
<Group name="ScreenMode" enabledWhen="platform.desktop">...</Group>
```

Runtime scripts read the same thing as `ctx.platform` (`{os, mobile}`).

### Verification checklist for the first device run

1. The APK installs, the title screen appears, and logcat shows
   `Mounted archive 'Content.pdx' (… source=sdl)`.
2. A full branch of the `minimal` template plays to the end.
3. Pressing Home during dialogue saves to the autosave slot, audio stops,
   and returning resumes audio and the text.
4. Swiping the app away from recents, then relaunching, lets you load the
   autosave.
5. Video plays, pauses when backgrounded, and resumes.
6. Every gesture in the touch table works, and taps land on the control
   under the finger even when the game is letterboxed.
7. With self-voicing on, each dialogue line is spoken.

Record what broke and how you fixed it here.

## Verifying a runtime bundle release

Native build instructions are in the [contributor section](../README.md#building-the-engine). Release engineers run:

```bash
node scripts/phase1-clean-install.mjs --mock --keep
node scripts/phase1-clean-install.mjs --runtime /path/to/installed-runtime --keep
```

Here `--runtime` supplies binaries for the candidate npm package; it is never passed to the simulated author CLI. The harness serves candidate packages through a loopback registry, installs the CLI globally with pnpm (`--package-manager npm` tests the npm path instead) into a temporary prefix, creates the project with the same package manager, watches edit/error/repair, closes the registry, reopens offline, builds and launches Player. Failed workspaces are retained; `--keep` also retains success evidence. Mock proves orchestration only. Native acknowledgements and launch do not prove visual interaction, audio or glyph correctness.

Windows x64, Linux x64 and macOS x64/arm64 are intended desktop targets. Each exact platform/architecture needs its own candidate evidence. WASM Preview is separate from Web game distribution. Web export ([web-export.md](web-export.md)) is experimental; mobile and console shipping are outside this desktop release.
