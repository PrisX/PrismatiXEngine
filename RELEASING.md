# Releasing PrismatiX

A release is one version shared by `@prismatix/cli`, `@prismatix/authoring-sdk`,
`@prismatix/runtime`, the native runtime packages
`@prismatix/native-win32-x64`, `@prismatix/native-linux-x64`,
`@prismatix/native-darwin-x64` and `@prismatix/native-darwin-arm64`, the Web
Player package `@prismatix/web-player`, and a GitHub Release. Authors install
only `@prismatix/cli`: it lists the native packages as exact-version
`optionalDependencies` (added by `scripts/release-packages.mjs` when it packs
the CLI, so `packages/cli/package.json` does not list them), and pnpm and npm
install the one matching the machine. `@prismatix/web-player` is not a
dependency; a project adds it to export for the web. The native and Web Player
`package.json` files are generated from the CLI's version when they are packed.
Pushing a tag `vX.Y.Z` runs [.github/workflows/release.yml](.github/workflows/release.yml),
which does all of it. Nothing is published from a developer machine.

## What the release workflow does

1. **version**: the tag must equal every package version
   (`node scripts/verify-release-version.mjs --tag vX.Y.Z` also checks the
   exact cross-pins, CMake, vcpkg.json, the starter template, the Starter
   sample, the VS Code extension, `CHANGELOG.md` and
   `docs/releases/X.Y.Z.md`), and the tagged commit must be on `master`.
   `node scripts/check-sample-provenance.mjs --release` must pass: every file
   under `Samples/` listed in its `SAMPLE_PROVENANCE.json` with a settled
   licence. A licence recorded as pending (`"license": "TBD"`) blocks the tag;
   the project owner's AI-generated pictures for samples, recorded as
   `"sample-only"`, pass for their exact bytes only.
2. **javascript**: `pnpm run check` (contracts, every TypeScript package test,
   the mock clean-install journey, docs examples).
3. **native**, on Windows x64, Linux x64, macOS x64 and macOS arm64: the same
   build, CTest suite and runtime-bundle steps as the quality gate
   (`.github/actions/*`) in a Release build; on Linux and macOS the installed
   binaries are stripped (`assemble-release.mjs strip-runtime`; the symbols go
   to a `-debug-symbols.tar.gz` archive), then the clean-install author
   journey with pnpm and with npm, then the runtime archive.
   **web-player**, on Linux: both Web Player renderers (WebGL2, WebGPU) with
   the pinned Emscripten (`.github/actions/web-player-build`,
   `scripts/build-wasm-player.mjs --no-host-tools`).
4. **assemble**: `node scripts/assemble-release.mjs assemble` packs the npm
   packages in publish order (the four native packages, `@prismatix/web-player`,
   authoring-sdk, runtime, cli; `out/release/release.json` records it),
   checks each tarball's contents (LICENSE, README, notices, each native
   package's manifest, tools and `os`/`cpu`, the CLI's optional dependencies
   and that it carries no runtime, the Web Player's version and hashes), prints
   each tarball's packed and unpacked size and writes `SHA256SUMS`.
5. **acceptance**, on all four platforms: a clean global install of the CLI
   from those exact tarballs (pnpm, then npm), which must bring this
   platform's native package and no other, create, dev, build and a packaged
   Player launch; `pack --target web` must fail with the install command, then
   `prismatix add web-player` installs `@prismatix/web-player` and the web
   export succeeds.
6. **github-release**: a draft GitHub Release with `docs/releases/X.Y.Z.md` as
   its notes, the npm tarballs, the runtime archives, the Linux and macOS
   debug-symbol archives and `SHA256SUMS`.
7. **npm-publish**, in the `release` environment (waits for approval): checks
   `SHA256SUMS`, publishes every package in the order of `release.json`
   with npm provenance, the CLI last so it never names a package that is not
   on npm yet (a version already on npm is skipped, so a failed run can be
   re-run), then makes the GitHub Release public.

`workflow_dispatch` (Actions, Release, Run workflow) is a dry run: steps 1 to 5
and `npm publish --dry-run` of every package (its log shows each package's
packed and unpacked size), without a GitHub Release or a publish: only a tag
push sets `publish=true`, which the github-release and npm-publish jobs require.

## One-time setup

Do these once, before the first tag.

1. **Make the repository public** (Settings, General, Danger Zone, Change
   visibility). Public repositories get GitHub-hosted runners, including
   macOS, without a billing limit, and npm provenance links back to the
   public source. Read the public-repository audit notes first.
2. **npm scope**: sign in to npmjs.com and make sure you own the `prismatix`
   scope: create the free organization `prismatix` (Add Organization) if it
   does not exist. The packages publish publicly under it.
3. **First publish credentials**: npm can only attach a trusted publisher to
   a package that already exists, so the first release publishes with a
   token. Create a granular access token (Access Tokens, Generate New Token,
   Granular): packages and scopes read and write, limited to the `@prismatix`
   scope (or all packages for a not-yet-existing scope), with a short expiry.
   Add it as the secret `NPM_TOKEN` of the `release` environment (next step).
4. **The `release` environment**: Settings, Environments, New environment,
   name it `release`. Add yourself under Required reviewers, and under
   Deployment branches and tags allow only tags matching `v*`. Add the
   `NPM_TOKEN` secret here.
5. **After the first release**, for each of the eight packages on npmjs.com
   (`@prismatix/native-win32-x64`, `@prismatix/native-linux-x64`,
   `@prismatix/native-darwin-x64`, `@prismatix/native-darwin-arm64`,
   `@prismatix/web-player`, `@prismatix/authoring-sdk`, `@prismatix/runtime`
   and `@prismatix/cli`): Settings, Trusted Publisher, GitHub Actions, with organization or user `PrisX`,
   repository `PrismatiXEngine`, workflow filename `release.yml` and
   environment `release`. Then delete the `NPM_TOKEN` secret and revoke the
   token; set each package's publishing access to "Require two-factor
   authentication and disallow tokens". Later releases publish through OIDC
   with no stored secret. A package added to the release later (a new
   platform, say) has no trusted publisher yet either: its first publish needs
   `NPM_TOKEN` again, then the same Trusted Publisher setup.

## Each release

1. Update the version everywhere it is stated (the CLI's `package.json` is the
   source): the three `package.json` files and their exact `@prismatix/*`
   pins (the native and Web Player packages take the CLI's version when they
   are packed), `packages/vscode-pxstory/package.json` (and the language server's
   `serverInfo`), `packages/cli/templates/minimal/package.json`,
   `Samples/Starter/package.json`,
   `cmake/PrismatiXContractVersions.cmake`, `vcpkg.json`,
   `ASSET_PROVENANCE.json` (`release`) and the exact pin in
   `docs/releases/delivery-contract.md`. Run `pnpm install` to refresh
   `pnpm-lock.yaml`.
2. Move the `[Unreleased]` notes in `CHANGELOG.md` to a dated `[X.Y.Z]`
   section and write `docs/releases/X.Y.Z.md`.
3. `node scripts/verify-release-version.mjs` and
   `node scripts/check-sample-provenance.mjs --release` must pass, and
   `pnpm test` and `ctest` must pass locally. When PrismatiX Studio's default
   new project changed, regenerate the sample first with
   `node scripts/refresh-starter-sample.mjs --studio <Studio checkout>`.
4. Commit, then push and tag:

   ```bash
   git push origin master
   git tag -s vX.Y.Z -m "PrismatiX X.Y.Z"   # or: git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

5. Watch the Release workflow; approve the `npm-publish` job when it asks.

If a platform fails after the tag was pushed, fix it on `master`, delete the
draft release and the tag (`git push --delete origin vX.Y.Z`), and tag again.
Once a version is on npm it can never be published again; release a new patch
version instead.

## Assembling locally

To inspect the packages without publishing, build the engine, install the
runtime component and pack with only your platform (or take the `runtime-<platform>`
and `web-player` artifacts of a Release workflow run with `gh run download`):

```bash
pnpm install && pnpm run build
cmake --install out/build/x64-release --prefix out/release/runtime --component PrismatiXRuntime
node scripts/assemble-release.mjs strip-runtime --runtime out/release/runtime --symbols out/release/archives   # Linux, macOS
node scripts/assemble-release.mjs archive-runtime --runtime out/release/runtime --out out/release/archives
node scripts/build-wasm-player.mjs --no-host-tools --package-root out/release/web-player   # optional; see docs/web-export.md
node scripts/assemble-release.mjs assemble --runtimes out/release/archives --web-player out/release/web-player --out out/release/dist --allow-partial
node scripts/phase1-clean-install.mjs --candidates out/release/dist/npm   # clean global install of those tarballs
```
