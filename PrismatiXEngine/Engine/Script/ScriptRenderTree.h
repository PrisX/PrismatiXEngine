#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::graphics {
class Renderer2D;
}

namespace px::script {

enum class RenderPhase : std::uint8_t {
    BeforeStage,
    AfterStage,
    BeforeUI,
    AfterUI,
};

[[nodiscard]] std::optional<RenderPhase> ParseRenderPhase(
    std::string_view value);
[[nodiscard]] std::string_view RenderPhaseName(RenderPhase value);

struct ScriptRenderNode {
    enum class Kind : std::uint8_t { Image, Rectangle, RoundedRectangle, Text };

    std::string id;
    Kind kind = Kind::Image;
    RenderPhase phase = RenderPhase::AfterStage;
    std::int64_t order = 0;
    bool visible = true;
    std::string route;

    std::string path;
    std::string text;
    std::string font;
    int fontSize = 24;
    Rect bounds{};
    std::optional<Rect> source;
    std::optional<Rect> clip;
    Color color{255, 255, 255, 255};
    float translateX = 0.0f;
    float translateY = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float rotationDegrees = 0.0f;
    float pivotX = 0.0f;
    float pivotY = 0.0f;
    float radius = 0.0f;
};

// Host-owned retained plans. Nodes contain resource paths and logical drawing
// data only; no SDL/GPU handle crosses the extension boundary.
class ScriptRenderTree {
public:
    static constexpr std::size_t kMaximumNodes = 4096;

    Status Upsert(ScriptRenderNode node);
    [[nodiscard]] bool Remove(std::string_view id);
    void Clear();
    [[nodiscard]] std::size_t Size() const { return m_nodes.size(); }
    void Render(graphics::Renderer2D& renderer, RenderPhase phase,
                std::string_view currentRoute = {},
                std::string_view currentModalRoute = {}) const;

    [[nodiscard]] Variant CaptureState() const;
    Status RestoreState(const Variant& state);
    [[nodiscard]] static Result<ScriptRenderNode> ParseNode(
        std::string_view id, const Variant& plan);

private:
    std::unordered_map<std::string, ScriptRenderNode> m_nodes;
};

}  // namespace px::script
