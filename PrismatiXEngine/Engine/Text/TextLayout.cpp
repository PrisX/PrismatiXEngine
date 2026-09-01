#include "Engine/Text/TextLayout.h"

#include "Engine/Graphics/AssetCache.h"
#include "Engine/Text/Typography.h"

#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace px::text {
namespace {

struct BidiResolution {
    bool baseRightToLeft = false;
    std::vector<std::pair<std::size_t, bool>> byteDirections;
};

BidiResolution ResolveBidi(const std::string_view text,
                           const bool localeRightToLeft) {
    const BidiLayoutInfo resolved =
        ResolveBidiDirections(text, localeRightToLeft);
    BidiResolution result;
    result.baseRightToLeft = resolved.baseRightToLeft;
    result.byteDirections.reserve(resolved.directions.size());
    for (const auto& direction : resolved.directions)
        result.byteDirections.emplace_back(direction.byteStart,
                                           direction.rightToLeft);
    return result;
}

bool DirectionAtByte(const BidiResolution& resolution,
                     const std::size_t byteOffset,
                     const bool fallback) {
    const auto found = std::upper_bound(
        resolution.byteDirections.begin(), resolution.byteDirections.end(),
        byteOffset, [](const std::size_t value, const auto& entry) {
            return value < entry.first;
        });
    return found == resolution.byteDirections.begin()
               ? fallback
               : std::prev(found)->second;
}

}  // namespace

TextLayout::TextLayout(std::string text, std::string locale, const Vec2 size,
                       std::vector<TextClusterLayout> clusters,
                       const TextOrientation orientation)
    : m_text(std::move(text)), m_locale(std::move(locale)), m_size(size),
      m_clusters(std::move(clusters)), m_orientation(orientation) {}

std::size_t TextLayout::ByteOffsetAt(const Vec2 point) const {
    if (m_clusters.empty()) return 0;
    const TextClusterLayout* nearest = &m_clusters.front();
    float nearestDistance = std::numeric_limits<float>::max();
    for (const auto& cluster : m_clusters) {
        const float left = cluster.bounds.x;
        const float right = cluster.bounds.x + cluster.bounds.w;
        const float top = cluster.bounds.y;
        const float bottom = cluster.bounds.y + cluster.bounds.h;
        if (point.x >= left && point.x <= right && point.y >= top && point.y <= bottom) {
            const bool after = m_orientation == TextOrientation::Vertical
                                   ? point.y >= top + cluster.bounds.h * 0.5f
                                   : point.x >= left + cluster.bounds.w * 0.5f;
            return after
                       ? cluster.byteStart + cluster.byteLength
                       : cluster.byteStart;
        }
        const float dx = point.x < left ? left - point.x : point.x > right ? point.x - right : 0.0f;
        const float dy = point.y < top ? top - point.y : point.y > bottom ? point.y - bottom : 0.0f;
        const float distance = dx * dx + dy * dy;
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = &cluster;
        }
    }
    const bool after = m_orientation == TextOrientation::Vertical
                           ? point.y >= nearest->bounds.y + nearest->bounds.h * 0.5f
                           : point.x >= nearest->bounds.x + nearest->bounds.w * 0.5f;
    return after
               ? nearest->byteStart + nearest->byteLength
               : nearest->byteStart;
}

Vec2 TextLayout::CaretPosition(const std::size_t byteOffset) const {
    if (m_clusters.empty()) return {};
    for (const auto& cluster : m_clusters) {
        const std::size_t end = cluster.byteStart + cluster.byteLength;
        if (byteOffset <= cluster.byteStart) {
            if (m_orientation == TextOrientation::Vertical)
                return {cluster.bounds.x, cluster.bounds.y};
            return {cluster.rightToLeft ? cluster.bounds.x + cluster.bounds.w
                                        : cluster.bounds.x,
                    cluster.bounds.y};
        }
        if (byteOffset <= end) {
            if (m_orientation == TextOrientation::Vertical)
                return {cluster.bounds.x,
                        cluster.bounds.y + cluster.bounds.h};
            return {cluster.rightToLeft ? cluster.bounds.x
                                        : cluster.bounds.x + cluster.bounds.w,
                    cluster.bounds.y};
        }
    }
    const auto& last = m_clusters.back();
    if (m_orientation == TextOrientation::Vertical)
        return {last.bounds.x, last.bounds.y + last.bounds.h};
    return {last.rightToLeft ? last.bounds.x
                             : last.bounds.x + last.bounds.w,
            last.bounds.y};
}

Rect TextLayout::BoundsForRange(const std::size_t byteStart,
                                const std::size_t byteLength) const {
    const std::size_t end = byteStart + byteLength;
    bool found = false;
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    for (const auto& cluster : m_clusters) {
        const std::size_t clusterEnd = cluster.byteStart + cluster.byteLength;
        if (clusterEnd <= byteStart || cluster.byteStart >= end) continue;
        if (!found) {
            left = cluster.bounds.x;
            top = cluster.bounds.y;
            right = cluster.bounds.x + cluster.bounds.w;
            bottom = cluster.bounds.y + cluster.bounds.h;
            found = true;
        } else {
            left = std::min(left, cluster.bounds.x);
            top = std::min(top, cluster.bounds.y);
            right = std::max(right, cluster.bounds.x + cluster.bounds.w);
            bottom = std::max(bottom, cluster.bounds.y + cluster.bounds.h);
        }
    }
    return found ? Rect{left, top, right - left, bottom - top} : Rect{};
}

TextLayoutService::TextLayoutService(graphics::AssetCache& assets)
    : m_assets(assets), m_engine(TTF_CreateSurfaceTextEngine()) {}

TextLayoutService::~TextLayoutService() {
    if (m_engine) TTF_DestroySurfaceTextEngine(m_engine);
}

void TextLayoutService::SetLocale(std::string locale) {
    m_locale = locale.empty() ? "und" : std::move(locale);
}

bool TextLayoutService::LocaleIsRightToLeft() const {
    const auto separator = m_locale.find_first_of("-_");
    const std::string language = m_locale.substr(0, separator);
    static const std::unordered_set<std::string> rtl{
        "ar", "dv", "fa", "he", "ku", "ps", "sd", "ug", "ur", "yi"};
    return rtl.contains(language);
}

void TextLayoutService::ConfigureFont(TTF_Font* font,
                                      const std::string_view text) const {
    if (!font) return;
    const BidiResolution bidi = ResolveBidi(text, LocaleIsRightToLeft());
    (void)TTF_SetFontLanguage(font, m_locale.c_str());
    (void)TTF_SetFontDirection(font, bidi.baseRightToLeft ? TTF_DIRECTION_RTL
                                                          : TTF_DIRECTION_LTR);
}

TextLayout TextLayoutService::Layout(const std::string_view value,
                                     const std::string& fontPath, const int size,
                                     const int wrapWidth,
                                     const TextOrientation orientation,
                                     std::size_t verticalRows) const {
    const std::string input(value);
    TTF_Font* font = m_assets.Font(fontPath, std::max(1, size), 0);
    if (!font || input.empty()) return {input, m_locale, {}, {}};
    const BidiResolution bidi = ResolveBidi(input, LocaleIsRightToLeft());
    ConfigureFont(font, input);

    if (orientation == TextOrientation::Vertical) {
        const auto boundaries = GraphemeBoundaries(input);
        const int advance = std::max(1, TTF_GetFontLineSkip(font));
        const std::size_t count = boundaries.size() > 1 ? boundaries.size() - 1 : 0;
        if (verticalRows == 0 && wrapWidth > 0)
            verticalRows = std::max<std::size_t>(
                1, static_cast<std::size_t>(wrapWidth / advance));
        if (verticalRows == 0) verticalRows = std::max<std::size_t>(1, count);
        struct PendingCluster {
            std::size_t start = 0;
            std::size_t length = 0;
            std::size_t column = 0;
            std::size_t row = 0;
            bool rotated = false;
        };
        std::vector<PendingCluster> pending;
        pending.reserve(count);
        std::size_t column = 0;
        std::size_t row = 0;
        std::size_t maxRows = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t start = boundaries[index];
            const std::size_t length = boundaries[index + 1] - start;
            const std::string_view glyph = value.substr(start, length);
            if (glyph == "\n" || glyph == "\r\n") {
                if (row > 0) ++column;
                row = 0;
                continue;
            }
            if (row >= verticalRows) {
                ++column;
                row = 0;
            }
            const bool rotate = glyph.size() == 1 && glyph.front() >= 0x21 &&
                                glyph.front() <= 0x7e;
            pending.push_back({start, length, column, row, rotate});
            ++row;
            maxRows = std::max(maxRows, row);
        }
        const std::size_t columns = pending.empty() ? 0 : column + 1;
        std::vector<TextClusterLayout> clusters;
        clusters.reserve(pending.size());
        for (const auto& item : pending) {
            clusters.push_back({item.start, item.length,
                                static_cast<int>(item.column),
                                {static_cast<float>((columns - item.column - 1) * advance),
                                 static_cast<float>(item.row * advance),
                                 static_cast<float>(advance), static_cast<float>(advance)},
                                item.rotated, false});
        }
        return {input, m_locale,
                {static_cast<float>(columns * advance),
                 static_cast<float>(maxRows * advance)},
                std::move(clusters), TextOrientation::Vertical};
    }

    if (!m_engine) {
        int width = 0, height = 0;
        if (wrapWidth > 0)
            (void)TTF_GetStringSizeWrapped(font, input.c_str(), input.size(), wrapWidth, &width, &height);
        else
            (void)TTF_GetStringSize(font, input.c_str(), input.size(), &width, &height);
        return {input, m_locale, {static_cast<float>(width), static_cast<float>(height)}, {}};
    }

    TTF_Text* shaped = TTF_CreateText(m_engine, font, input.c_str(), input.size());
    if (!shaped) return {input, m_locale, {}, {}};
    if (wrapWidth > 0) (void)TTF_SetTextWrapWidth(shaped, wrapWidth);
    int width = 0, height = 0;
    (void)TTF_GetTextSize(shaped, &width, &height);
    std::vector<TextClusterLayout> clusters;
    TTF_SubString current{};
    if (TTF_GetTextSubString(shaped, 0, &current)) {
        for (;;) {
            if (current.length > 0) {
                clusters.push_back({static_cast<std::size_t>(current.offset),
                                    static_cast<std::size_t>(current.length), current.line_index,
                                    {static_cast<float>(current.rect.x), static_cast<float>(current.rect.y),
                                     static_cast<float>(current.rect.w), static_cast<float>(current.rect.h)},
                                    false,
                                    DirectionAtByte(
                                        bidi,
                                        static_cast<std::size_t>(current.offset),
                                        (current.flags &
                                         TTF_SUBSTRING_DIRECTION_MASK) ==
                                            TTF_DIRECTION_RTL)});
            }
            if ((current.flags & TTF_SUBSTRING_TEXT_END) != 0) break;
            TTF_SubString next{};
            if (!TTF_GetNextTextSubString(shaped, &current, &next)) break;
            if (next.offset == current.offset && next.length == current.length) break;
            current = next;
        }
    }
    TTF_DestroyText(shaped);
    return {input, m_locale, {static_cast<float>(width), static_cast<float>(height)},
            std::move(clusters)};
}

}  // namespace px::text
