#pragma once

#include "Engine/Core/Types.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::text {
struct RubySpan { std::string prefix; std::string base; std::string reading; };
// Inline style of a run of dialogue text. Unset fields inherit the control's
// font, size and colour; `size` is absolute logical pixels and `scale`
// multiplies whichever size applies.
struct TextRunStyle {
    std::string font;
    int size = 0;
    float scale = 1.0f;
    std::optional<Color> color;
    bool bold = false;
    bool italic = false;
    [[nodiscard]] bool operator==(const TextRunStyle&) const = default;
    [[nodiscard]] bool Plain() const { return *this == TextRunStyle{}; }
};
struct TextRun { std::size_t byteStart = 0; std::size_t byteLength = 0; TextRunStyle style; };
// `runs` cover `plain` end to end when the markup has any style tag and are
// empty otherwise.
struct RichText { std::string plain; std::vector<RubySpan> ruby; std::vector<TextRun> runs; };
struct BidiDirection {
    std::size_t byteStart = 0;
    bool rightToLeft = false;
};
struct BidiLayoutInfo {
    bool baseRightToLeft = false;
    std::vector<BidiDirection> directions;
};
// Reads `[br]`, `[ruby=reading]base[/ruby]` and the style tags `[font=path]`,
// `[size=N]`/`[size=N%]`, `[color=#RRGGBB(AA)]`, `[b]` and `[i]` (each closed
// by `[/name]`, unclosed ones last to the end). Other bracketed tags are
// hidden.
[[nodiscard]] RichText ParseRubyMarkup(std::string_view markup);
// Length of the style tag (open or close) at the start of `text`, else 0.
[[nodiscard]] std::size_t StyleTagLength(std::string_view text);
// A font path that selects synthesized bold and/or italic glyphs; see
// AssetCache::Font.
[[nodiscard]] std::string StyledFontPath(std::string_view path, bool bold, bool italic);
// Unicode bidi levels keyed by UTF-8 byte offsets. TextLayoutService is the
// consumer that turns these directions into immutable cluster geometry.
[[nodiscard]] BidiLayoutInfo ResolveBidiDirections(
    std::string_view text, bool localeRightToLeft = false);
// UTF-8 byte offsets for extended grapheme clusters. The first element is
// always zero and the final element is text.size().
[[nodiscard]] std::vector<std::size_t> GraphemeBoundaries(std::string_view text);
// UTF-8 byte offsets where Unicode UAX #14 permits or requires a line break.
// Kinsoku consumers may further restrict these boundaries for VN typography.
[[nodiscard]] std::vector<std::size_t> LineBreakBoundaries(
    std::string_view text, std::string_view language = {});
[[nodiscard]] std::string ApplyCjkKinsoku(std::string_view text,std::size_t maxColumns);
// Byte offsets where a line break must be inserted so every line of `text`
// fits `width` as reported by `measure`: UAX #14 opportunities, with 禁則 (no
// closing punctuation or small kana at a line start, no opening bracket at a
// line end). Existing newlines are kept and not reported.
[[nodiscard]] std::vector<std::size_t> WrapBreaks(
    std::string_view text, float width,
    const std::function<float(std::string_view)>& measure,
    std::string_view language = "ja");
struct VerticalGlyph { std::string glyph; int column=0; int row=0; bool rotate=false; };
[[nodiscard]] std::vector<VerticalGlyph> LayoutVertical(std::string_view text,std::size_t rowsPerColumn);
}  // namespace px::text
