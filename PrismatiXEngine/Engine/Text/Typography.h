#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace px::text {
struct RubySpan { std::string prefix; std::string base; std::string reading; };
struct RichText { std::string plain; std::vector<RubySpan> ruby; };
[[nodiscard]] RichText ParseRubyMarkup(std::string_view markup);
[[nodiscard]] std::string ApplyCjkKinsoku(std::string_view text,std::size_t maxColumns);
struct VerticalGlyph { std::string glyph; int column=0; int row=0; bool rotate=false; };
[[nodiscard]] std::vector<VerticalGlyph> LayoutVertical(std::string_view text,std::size_t rowsPerColumn);
}  // namespace px::text
