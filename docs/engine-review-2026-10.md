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

## Open, larger items

2. **Save management.** Senren/Sakura offer copy, swap, delete and an editable
   comment per slot; the Player has no deletion at all (Sakura's port omits its
   `del` dialog for that reason). Add `save.delete`, `save.copy`,
   `save.move` actions with confirmations, and a `comment` slot field.
3. **Live preview in settings.** Artemis shows sample text typing at the chosen
   speed and auto-advancing. Publish a `settings.preview` typewriter string the
   Config screen can bind.
4. **Glyph coverage beyond Story.** `PXCLI1060` checks Story text; TSX `text`,
   locale `strings` and button captions should be checked the same way.
5. **Kinsoku for styled and ruby lines.** Lines with inline style runs use the
   UAX #14 run layout (correct); lines with ruby keep SDL_ttf wrapping so ruby
   offsets stay valid. Moving ruby onto the run layout removes the exception.
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
