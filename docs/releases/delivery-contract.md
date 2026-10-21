# PrismatiXEngine delivery contract

PrismatiXEngine 0.1.0 is the first versioned public authoring/runtime boundary.
The intended desktop targets are Windows x64, Linux x64 and macOS
x64/arm64 Player bundles produced by the matching Packager. Each architecture
requires its own candidate installation and native acceptance evidence before
it is declared supported. Emscripten has separate Studio Preview and shipping
Web Player targets. `pack --target web` selects the shipping runtime explicitly;
Preview cannot substitute for it. See [Web export](../web-export.md) for build,
hosting and browser acceptance limits. Native Android remains experimental;
console targets are not supported by this release.

## Compatibility

- This candidate pins CLI, authoring SDK, the native runtime packages and the
  Web Player package to exact `0.1.0`.
  The global command delegates project operations to their pinned local CLI;
  missing or mismatched local installations are diagnosed. The runtime SDK
  belongs to the same contract line, but a minor-line match alone is not proof
  of native ABI compatibility.
- Package archives remain PDX5. The 0.1 writer streams content with bounded
  buffers; the on-disk header, index and reader contract are unchanged.
- Every document format and wire contract (project, Runtime IR, source map,
  UI, extension, preview protocol and the rest) is at revision 1. Only
  revision 1 is accepted; there is no migration of other revisions.
- Save compatibility is declared by `contentVersion`, `saveVersion`, stable
  persistence identity and packaged migration chains. A release update must
  pass the V1-to-V2 package/save journey before publication.

## Release contents

The author-facing CLI tarball carries no native runtime. Each platform's runtime
is its own package, `@prismatix/native-<platform>-<architecture>` (the runtime
bundle at the package root, with `os`/`cpu` fields), which the CLI lists as an
exact-version optional dependency and resolves from its installation. A
generated manifest is not proof of inclusion: inspect and install the actual
candidate tarballs and check that the author's machine got its native package.
The Web Player is the separate `@prismatix/web-player` package, installed into
a project on demand at the CLI's exact version. Create
prepares and locks the local toolchain automatically. No packages were publicly
published by this audit.

A desktop runtime bundle contains `bin/` tools and their private runtime
dependencies, `share/PrismatiX/runtime-manifest.json`, this delivery contract,
the root MIT license, `THIRD_PARTY_NOTICES.md`, `ASSET_PROVENANCE.json`, and
collected dependency copyright files. A packaged game copies the release notice
inventory next to the Player under `Notices/`.

## Required gates

1. Build and test the native Runtime, SDK, Packager and Player.
2. Install the `PrismatiXRuntime` component into a fresh prefix.
3. Run the clean-install author journey with repository and vcpkg discovery
   removed: candidate global install, create, persistent dev/edit/error/repair,
   registry shutdown, offline reopen/build, then packaged Player launch.
   The launch smoke is separate from visual/audio interaction acceptance.
4. Run deterministic PDX packaging, input-mutation and large streaming-media
   coverage.
5. For the separately shipped WASM Preview artifact, build it and run both
   Node protocol conformance and the real headless-browser canvas/audio/input
   smoke. Its unverified status must be explicit; it is not a prerequisite for
   a release restricted to the verified desktop SDK targets.
6. Verify notices, font hashes and sample provenance before publishing an
   artifact. Every file under `Samples/` is listed with its SHA-256, origin
   and licence in its sample's `SAMPLE_PROVENANCE.json`;
   `node scripts/check-sample-provenance.mjs --release` fails on an unlisted
   or changed file and on any licence still pending; the project owner's
   AI-generated pictures for samples pass as `sample-only`, for their exact
   bytes only.
7. For a Web release, verify shipping runtime hashes and notices, real-browser
   canvas/media/effect behavior, persistent saves and migration, complete offline
   playback and accepted updates. Record tested browsers/devices explicitly.

Debug builds and repository-local executions are useful diagnostics, but they
do not substitute for these release gates.

## Author-facing changes

`build --output <path>` now remains a distribution build. Automation requiring
canonical compiler inputs must add `--artifact-only`; this is an intentional
behavior change from implicit output-based selection. Existing arbitrary
directories and author inputs are not valid promotion targets.

Dev writes immutable revisions and starts a replacement Preview before retiring
the previous one. The current update policy restarts the scene, including when
a text-only edit could theoretically be patched in place. This makes the
acknowledged runtime and its on-disk resources a single revision; it is not a
claim that live progress preservation is complete. Superseded or rejected revision directories are retired only after confirmed Host closure; the last acknowledged revision remains for diagnostics.


Preview Title/HUD Save/Load uses session-only checkpoints cleared on rebuild or Host close. Player saves persist on disk. The bounded Preview uiControls diagnostic snapshot is partial (at most 64 controls and 512 text bytes per control), not a complete UI serialization or persistence contract.
