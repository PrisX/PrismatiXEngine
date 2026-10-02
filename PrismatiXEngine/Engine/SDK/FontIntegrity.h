#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace px::sdk {

// Checks the table directory of a TrueType/OpenType font (or each face of a
// collection): every table must lie inside the file and match its checksum.
// FreeType ignores both, so a font damaged in transit (for example by a text
// tool converting line endings) still opens but draws no glyphs. Returns an
// empty string for a sound font and for data that is not sfnt at all, which
// font loading rejects on its own; otherwise a description of the damage.
[[nodiscard]] std::string FindFontFileDamage(std::span<const std::uint8_t> bytes);

}  // namespace px::sdk
