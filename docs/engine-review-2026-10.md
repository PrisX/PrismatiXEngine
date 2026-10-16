# Engine review, October 2026

Findings from building a second game with the SDK (`Projects/Unnamable-Regret`,
chapter 1 of an original script with a custom UI), next to the Sakura no Uta
port. Fixed items are in the commit history; this file keeps the larger work
that is still open, roughly in priority order.

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

## Open, larger items

6. **Scene fall-through.** Every scene ends with an explicit
   `[jump scene=next]`. Option: `prismatix add scene` writes the jump, or a
   chapter's scene order is followed when a scene reaches its end.
7. **QA journeys.** A tour of every screen exceeds the 180 s journey timeout
   when it clicks through text; add `px.advanceUntil(condition)` on the Player
   side (one round trip) and a per-journey timeout. `px.save` stores no
   thumbnail: the IPC handler runs outside the frame, so it would have to queue
   the save for the frame loop the way the Save screen captures its image.
8. **A generic script importer.** The game's `tools/import-hackmd.mjs` turns a
   writers' Markdown draft (scene headings, speaker tags, cut passages,
   letters) plus a staging table into `.pxstory`. Writers commonly draft this
   way; a configurable `prismatix import screenplay` would serve every
   project.
9. **Documentation.** `docs/authoring.md` is ~2,800 lines mixing tutorial and
   implementation detail (cache sizes, hashing, retained-artifact rules). Split
   it into a getting-started tutorial, a Story reference, a UI reference and a
   shipping/maintenance reference.
