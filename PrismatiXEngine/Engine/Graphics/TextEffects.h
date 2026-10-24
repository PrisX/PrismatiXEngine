#pragma once

#include "Engine/Core/Types.h"

namespace px::graphics {

// Optional looks for text drawn with DrawTextInRect, in draw order shadow,
// outline, fill. The default sets none: the text draws exactly as without
// it, at the same cost. Shadow, outline and bold are baked into cached glyph
// textures and slant and gradient go in the geometry, so a styled label costs
// one draw per layer per frame.
struct TextEffects {
    // A drop shadow of the glyphs and their outline; alpha 0 is none.
    Color shadowColor{0, 0, 0, 0};
    float shadowOffsetX = 2.0f;
    float shadowOffsetY = 2.0f;
    // Softness in pixels, 0..8.
    float shadowBlur = 0.0f;
    // The fill runs from `gradientFrom` to `gradientTo` instead of the text
    // colour: down each line, or across the text when horizontal.
    bool gradient = false;
    Color gradientFrom{255, 255, 255, 255};
    Color gradientTo{255, 255, 255, 255};
    bool gradientHorizontal = false;
    // A horizontal gradient spans these x coordinates when right > left (a
    // caption drawn in pieces passes its whole width); else the text's own.
    float gradientLeft = 0.0f;
    float gradientRight = 0.0f;
    // Faux bold: pixels added on each side of the glyphs, 0..4.
    float bold = 0.0f;
    // Faux italic: slant in degrees, -20..20; positive leans right about
    // each line's baseline.
    float italic = 0.0f;
    [[nodiscard]] bool Any() const {
        return shadowColor.a > 0 || gradient || bold > 0.0f || italic != 0.0f;
    }
};

}  // namespace px::graphics
