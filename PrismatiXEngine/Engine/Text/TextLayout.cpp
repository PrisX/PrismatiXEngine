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
    m_cache.Clear();
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
                                     const std::size_t verticalRows) const {
    std::string key;
    key.reserve(fontPath.size() + value.size() + 48);
    key.append(fontPath);
    key += '|';
    key += std::to_string(size);
    key += '|';
    key += std::to_string(wrapWidth);
    key += orientation == TextOrientation::Vertical ? "|v" : "|h";
    key += std::to_string(verticalRows);
    key += '|';
    key.append(value);
    TextLayout cached;
    if (m_cache.Get(key, cached)) return cached;
    TextLayout shaped = Shape(std::string(value), fontPath, size, wrapWidth,
                              orientation, verticalRows);
    m_cache.Put(key, shaped);
    return shaped;
}

TextLayout TextLayoutService::LayoutRuns(const std::string_view value,
                                         const std::vector<TextFontRun>& runs,
                                         const int wrapWidth) const {
    std::string key = "runs|" + std::to_string(wrapWidth);
    for (const auto& run : runs) {
        key += '|';
        key += std::to_string(run.byteStart) + ',' + std::to_string(run.byteLength) + ',' +
               std::to_string(run.size) + ',' + run.fontPath;
    }
    key += '|';
    key.append(value);
    TextLayout cached;
    if (m_cache.Get(key, cached)) return cached;

    struct RunMetrics { int ascent = 0; int height = 0; int lineSkip = 0; };
    struct Item { std::size_t start = 0; std::size_t length = 0; float width = 0.0f;
                  std::size_t run = 0; bool newline = false; };
    std::vector<RunMetrics> metrics;
    std::vector<Item> items;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const auto& run = runs[index];
        if (run.byteStart + run.byteLength > value.size()) break;
        RunMetrics metric;
        if (TTF_Font* font = m_assets.Font(run.fontPath, std::max(1, run.size), 0)) {
            metric = {TTF_GetFontAscent(font), TTF_GetFontHeight(font),
                      TTF_GetFontLineSkip(font)};
        }
        metrics.push_back(metric);
        const std::string_view text = value.substr(run.byteStart, run.byteLength);
        for (std::size_t piece = 0; piece <= text.size();) {
            // "\r\n" and a lone "\r" break the line like "\n".
            const auto end = std::min(text.find_first_of("\r\n", piece), text.size());
            if (end > piece) {
                // SDL_ttf cluster rects are ink boxes; a cluster's advance is
                // the growth of the measured prefix that ends with it.
                const std::string_view part = text.substr(piece, end - piece);
                const TextLayout shaped = Layout(part, run.fontPath, run.size, 0);
                std::vector<std::pair<std::size_t, std::size_t>> spans;
                for (const auto& cluster : shaped.Clusters())
                    spans.emplace_back(cluster.byteStart, cluster.byteLength);
                std::ranges::sort(spans);
                TTF_Font* font = m_assets.Font(run.fontPath, std::max(1, run.size), 0);
                if (font) ConfigureFont(font, part);
                const bool controls = HasControlCharacters(part);
                int previous = 0;
                for (const auto& [start, length] : spans) {
                    int width = 0, height = 0;
                    if (font && controls) {
                        const std::string prefix = MakeDrawable(part.substr(0, start + length));
                        (void)TTF_GetStringSize(font, prefix.data(), prefix.size(), &width, &height);
                    } else if (font) {
                        (void)TTF_GetStringSize(font, part.data(), start + length, &width, &height);
                    }
                    items.push_back({run.byteStart + piece + start, length,
                                     static_cast<float>(std::max(0, width - previous)), index, false});
                    previous = std::max(previous, width);
                }
            }
            if (end == text.size()) break;
            const std::size_t breakLength =
                text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n' ? 2 : 1;
            items.push_back({run.byteStart + end, breakLength, 0.0f, index, true});
            piece = end + breakLength;
        }
    }
    if (items.empty()) {
        m_cache.Put(key, TextLayout(std::string(value), m_locale, {}, {}));
        return TextLayout(std::string(value), m_locale, {}, {});
    }

    // The same opportunities as Label wrapping, so styled and ruby lines
    // follow 禁則 too.
    const auto breaks = KinsokuBreakBoundaries(value, m_locale);
    const auto breakBefore = [&](const std::size_t byte) {
        return std::binary_search(breaks.begin(), breaks.end(), byte);
    };
    // Greedy fill: a line that overflows breaks at its last opportunity, or
    // before the overflowing cluster when there is none.
    std::vector<std::vector<Item>> lines(1);
    float width = 0.0f;
    std::size_t lastBreak = 0;
    for (const auto& item : items) {
        auto& line = lines.back();
        if (item.newline) {
            line.push_back(item);
            lines.emplace_back();
            width = 0.0f;
            lastBreak = 0;
            continue;
        }
        if (!line.empty() && breakBefore(item.start)) lastBreak = line.size();
        if (wrapWidth > 0 && !line.empty() && width + item.width > static_cast<float>(wrapWidth)) {
            const std::size_t split = lastBreak > 0 ? lastBreak : line.size();
            std::vector<Item> carried(line.begin() + static_cast<std::ptrdiff_t>(split), line.end());
            line.resize(split);
            width = 0.0f;
            for (const auto& moved : carried) width += moved.width;
            lines.push_back(std::move(carried));
            lastBreak = !lines.back().empty() && breakBefore(item.start) ? lines.back().size() : 0;
        }
        lines.back().push_back(item);
        width += item.width;
    }
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();

    std::vector<TextClusterLayout> clusters;
    clusters.reserve(items.size());
    float top = 0.0f;
    float widest = 0.0f;
    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const auto& line = lines[lineIndex];
        int ascent = 0;
        int lineSkip = 0;
        for (const auto& item : line) {
            ascent = std::max(ascent, metrics[item.run].ascent);
            lineSkip = std::max(lineSkip, metrics[item.run].lineSkip);
        }
        if (line.empty()) {
            ascent = metrics.front().ascent;
            lineSkip = metrics.front().lineSkip;
        }
        float x = 0.0f;
        for (const auto& item : line) {
            const auto& metric = metrics[item.run];
            clusters.push_back({item.start, item.length, static_cast<int>(lineIndex),
                                {x, top + static_cast<float>(ascent - metric.ascent),
                                 item.width, static_cast<float>(metric.height)},
                                false, false});
            x += item.width;
        }
        widest = std::max(widest, x);
        top += static_cast<float>(lineSkip);
    }
    TextLayout shaped(std::string(value), m_locale, {widest, top}, std::move(clusters));
    m_cache.Put(key, shaped);
    return shaped;
}

TextLayout TextLayoutService::Shape(const std::string& input,
                                    const std::string& fontPath, const int size,
                                    const int wrapWidth,
                                    const TextOrientation orientation,
                                    const std::size_t verticalRows) const {
    if (!HasControlCharacters(input))
        return ShapeDrawable(input, fontPath, size, wrapWidth, orientation, verticalRows);
    // Control characters never reach the font: shape the drawable text and
    // map its clusters back onto the caller's byte offsets.
    const DrawableText drawable = MapDrawableText(input);
    const TextLayout shaped = ShapeDrawable(drawable.text, fontPath, size, wrapWidth,
                                            orientation, verticalRows);
    std::vector<TextClusterLayout> clusters;
    clusters.reserve(shaped.Clusters().size());
    for (auto cluster : shaped.Clusters()) {
        if (cluster.byteLength == 0 ||
            cluster.byteStart + cluster.byteLength > drawable.text.size())
            continue;
        const std::size_t start = drawable.sourceStart[cluster.byteStart];
        const std::size_t end = drawable.sourceEnd[cluster.byteStart + cluster.byteLength - 1];
        cluster.byteStart = start;
        cluster.byteLength = end - start;
        clusters.push_back(cluster);
    }
    return {input, m_locale, shaped.Size(), std::move(clusters), shaped.Orientation()};
}

TextLayout TextLayoutService::ShapeDrawable(const std::string& input,
                                            const std::string& fontPath, const int size,
                                            const int wrapWidth,
                                            const TextOrientation orientation,
                                            std::size_t verticalRows) const {
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
            const std::string_view glyph = std::string_view(input).substr(start, length);
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
