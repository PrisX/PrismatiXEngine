#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace px::text {
struct RubySpan { std::string prefix; std::string base; std::string reading; };
struct RichText { std::string plain; std::vector<RubySpan> ruby; };
struct BidiDirection {
    std::size_t byteStart = 0;
    bool rightToLeft = false;
};
struct BidiLayoutInfo {
    bool baseRightToLeft = false;
    std::vector<BidiDirection> directions;
};
[[nodiscard]] RichText ParseRubyMarkup(std::string_view markup);
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
struct VerticalGlyph { std::string glyph; int column=0; int row=0; bool rotate=false; };
[[nodiscard]] std::vector<VerticalGlyph> LayoutVertical(std::string_view text,std::size_t rowsPerColumn);
}  // namespace px::text
