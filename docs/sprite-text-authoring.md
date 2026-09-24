# Sprite lettering without one control per glyph

`SpriteText` displays short text using a declared set of image glyphs. It is a
typed TSX UI control; no separate descriptor format is required.

```tsx
import {Scene, SpriteText} from "@prismatix/authoring-sdk";

const digits = [
  {character: "1", path: "Assets/UI/digit-1.png",
   x: 0, y: 0, width: 18, height: 30},
  {character: "-", path: "Assets/UI/dash.png",
   x: 0, y: 8, width: 19, height: 10},
];

export default <Scene name="Chapter" width={420} height={143}>
  <SpriteText name="ChapterNumber" glyphs={digits} advance={24}
    size={[420, 143]} bind={{text: "chapter.number"}} />
</Scene>;
```

Each `character` is one Unicode grapheme. `x` and `y` are offsets within the
control, and `width`/`height` are the rendered dimensions. `advance` moves the
next glyph's origin by a fixed logical-pixel distance; zero uses each glyph's
width. Missing characters are left blank. Playback renders at most 64 graphemes
per control, so this is for badges, counters and decorative labels, not long
dialogue or general text shaping.

Authoring validates unique glyphs, bounds and safe image paths. Project builds
package every glyph image, even if it is not in the initial text. Native UI
preloading likewise includes the full glyph set before showing the page.
`text` is bindable and can change without rebuilding the UI tree. Glyphs are
separate image resources; atlas source rectangles and kerning are not yet part
of this control.
