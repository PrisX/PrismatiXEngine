#pragma once

#include "Engine/Core/Types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct TTF_Font;
struct TTF_TextEngine;

namespace px::graphics { class AssetCache; }

namespace px::text {

enum class TextOrientation { Horizontal, Vertical };

struct TextClusterLayout {
    std::size_t byteStart = 0;
    std::size_t byteLength = 0;
    int line = 0;
    Rect bounds{};
    bool rotated = false;
    bool rightToLeft = false;
};

// Backend-neutral immutable output shared by drawing, caret/hit testing,
// ruby placement, typewriter ranges, and accessibility adapters.
class TextLayout final {
public:
    TextLayout() = default;
    TextLayout(std::string text, std::string locale, Vec2 size,
               std::vector<TextClusterLayout> clusters,
               TextOrientation orientation = TextOrientation::Horizontal);

    [[nodiscard]] const std::string& Text() const { return m_text; }
    [[nodiscard]] const std::string& Locale() const { return m_locale; }
    [[nodiscard]] Vec2 Size() const { return m_size; }
    [[nodiscard]] const std::vector<TextClusterLayout>& Clusters() const { return m_clusters; }
    [[nodiscard]] TextOrientation Orientation() const { return m_orientation; }
    [[nodiscard]] std::size_t ByteOffsetAt(Vec2 point) const;
    [[nodiscard]] Vec2 CaretPosition(std::size_t byteOffset) const;
    [[nodiscard]] Rect BoundsForRange(std::size_t byteStart,
                                      std::size_t byteLength) const;

private:
    std::string m_text;
    std::string m_locale;
    Vec2 m_size{};
    std::vector<TextClusterLayout> m_clusters;
    TextOrientation m_orientation = TextOrientation::Horizontal;
};

class TextLayoutService final {
public:
    explicit TextLayoutService(graphics::AssetCache& assets);
    ~TextLayoutService();
    TextLayoutService(const TextLayoutService&) = delete;
    TextLayoutService& operator=(const TextLayoutService&) = delete;

    void SetLocale(std::string locale);
    [[nodiscard]] const std::string& Locale() const { return m_locale; }
    void ConfigureFont(TTF_Font* font, std::string_view text = {}) const;
    [[nodiscard]] TextLayout Layout(
        std::string_view value, const std::string& fontPath, int size,
        int wrapWidth = 0,
        TextOrientation orientation = TextOrientation::Horizontal,
        std::size_t verticalRows = 0) const;

private:
    [[nodiscard]] bool LocaleIsRightToLeft() const;

    graphics::AssetCache& m_assets;
    TTF_TextEngine* m_engine = nullptr;
    std::string m_locale = "und";
};

}  // namespace px::text
