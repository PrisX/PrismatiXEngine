# Engine review, October 2026

Findings from building a game with the SDK (`Projects/Unnamable-Regret`,
chapter 1 of an original script with a custom UI). Each item is in the commit history; the larger ones that were left open
at first moved from "Open, larger items" to the list below as they were done.

## Fixed while building

- Stock screens and starter: box decoration (rounded, bordered, gradient,
  shadowed panels without textures), redesigned title/HUD/save/load/backlog/
  gallery/settings, original starter art, a click-wait glyph.
- `create` works offline from an engine checkout; a checkout CLI finds its own
  native build.
- Single-locale projects lost their locale file (fontChain, strings).
- Fonts opened as fallbacks were reused without their own fallback chain, so
  missing glyphs drew as boxes; `validate` now warns about Story characters no
  font can draw.
- Kinsoku line breaking decided on the whole line; speaker name colours;
  legible read-text dimming.
- `RichTextLabel` dropped `font`/`letterSpacing`; Buttons gained `font`; TSX
  warns about props that do nothing.
- Expressions addressable by name; `add character` picks `normal.png` and
  allows sprite-less characters; `storyAssets` roots no longer need a
  matching `resourceIncludes`; CJK ideographic spaces no longer split command
  arguments; QA `jump` reaches in-scene labels.
- Controls without a font use the locale's first `fontChain` font unless
  `player.uiFont` is set, instead of a hard-coded Noto Sans TC path.
- Portrait blink and lip sync: `.pxportrait` expression layers take `blink`
  and `mouth` frame lists; the Stage blinks on a random 2.5-6 s timer and
  opens the speaking character's mouth with the voice track's RMS level
  (metered by a raw callback on the voice track). Not saved.
- Save management: `save.delete`, `save.copy`, `save.move` (a move onto a
  save swaps them; the next activated slot is the target), `save.comment`
  with a `SlotComment` field, and `deleteSave`/`copySave`/`moveSave`
  confirmations. Copies and moves are file operations, so saves stay
  byte-identical.
- `settings.preview` / `settings.previewWaiting`: a sample line typing at the
  chosen text speed and holding for the auto wait while settings are open;
  the stock settings screen shows it.
- `PXCLI1060` also checks UI text (labels, button captions, placeholders,
  with each node's own font and `player.uiFont`) and locale `strings`. It
  found `↺` in this game's backlog hint, which no Noto Serif face draws.
- Ruby lines use the run layout like styled lines (`Label::AllowsReflow` is
  gone), and the run layout applies the same 禁則 filter as plain-text
  wrapping (it used bare UAX #14 before). While the typewriter reveals a
  styled or ruby line, the whole line is laid out, so nothing jumps lines.
- Scene fall-through: `scene=next` resolves to the following scene in the
  index's chapter order (after the last scene `[jump scene=next]` ends the
  story), and `add scene` ends new scenes with it. Chosen over implicit
  fall-through at a scene's end because the compiler's scene boundary is
  deliberate: with several route endings in one chapter, a forgotten `[end]`
  would otherwise run one ending into the next. The marker keeps the order in
  the index, so reordering scenes needs no jump edits.
- QA journeys: `px.advanceUntil(condition)` clicks through lines on the
  Player side in one request; a journey can `export const timeoutMs`
  (`--timeout` still overrides); `px.save` keeps the request's state and
  writes it after the next Stage frame with that frame as thumbnail (inside a
  batch it stays synchronous). The tour here took 177.6 s of its 180 s, mostly
  in `stable` waits rather than text clicks.
- `prismatix import screenplay`: the HackMD importer's approach made
  configurable (heading and speaker patterns, alias table, narrators, cut
  markers, letter blocks, a staging table with `at`/`after` cues). Run on
  this game's draft with tables converted from `tools/import-hackmd.mjs`, it
  reproduces the Story except the importer's mood heuristics (expression
  guesses from narration), and the result passes the tour journey. Re-imports
  keep save identities of unchanged lines through the SDK's identity sync.
- Documentation: `docs/authoring.md` is now an index; the guide is split into
  `getting-started.md` (tutorial), `story-reference.md`, `ui-reference.md` and
  the shipping/maintenance reference `shipping.md`. Build-cache, hashing and
  provenance internals, the motion design proposal and M2 notes were dropped.

## Open, larger items

None at the moment.
