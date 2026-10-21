# Multi-locale VN test fixture

A small canonical project (`project.pxproject`) that the test suites build and
package. It is test input, not an example to copy; the SDK's example project
is `Samples/Starter`.

It covers aligned Traditional Chinese/Japanese Story sources with external
save identities (`.pxstory.pxids.json`), ADV/NVL commands, typed session and
profile variables, choices, a gallery unlockable, canonical `.pxui` Title, HUD
and Gallery screens with a UI component, an isolated JavaScript extension with
a module, a package-time GPU effect and a save migration.

Used by:

- `packages/cli/test/cli.test.ts`: validate and build of a multi-locale
  project; both locale programs must share one execution topology.
- `docs/examples/authoring-build.ts` (`pnpm run docs:test`): the documented
  Authoring SDK `compileProject` example.
- `PrismatiX.Player.CatalogNativeAcceptance` (CTest): its Title, HUD and
  Gallery `.pxui` drive the packaged Player journey.

## Provenance

Every file here was written for the PrismatiX test suite by its authors and is
covered by the repository's MIT license: the Story text, the JSON documents,
the extension scripts, the shader and `Assets/ending.svg` (a hand-written
vector gradient). It contains no third-party artwork, audio or fonts.
