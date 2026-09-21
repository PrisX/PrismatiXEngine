#include "Engine/Script/ScriptRenderTree.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/Renderer2D.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace px::script {
namespace {

diag::Diagnostic RenderDiagnostic(std::string code, std::string message,
                                  std::string details = {}) {
    return {.severity = diag::Severity::Error,
            .code = std::move(code),
            .category = "Script.Render",
            .message = std::move(message),
            .details = std::move(details)};
}

const Variant* Property(const VariantObject& object, const std::string_view key) {
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}

std::optional<double> Number(const VariantObject& object,
                             const std::string_view key) {
    const auto* value = Property(object, key);
    if (!value) return std::nullopt;
    if (const auto* number = value->TryGet<double>()) return *number;
    if (const auto* integer = value->TryGet<std::int64_t>())
        return static_cast<double>(*integer);
    return std::nullopt;
}

std::optional<std::string> String(const VariantObject& object,
                                  const std::string_view key) {
    const auto* value = Property(object, key);
    if (!value) return std::nullopt;
    if (const auto* string = value->TryGet<std::string>()) return *string;
    return std::nullopt;
}

bool FiniteRect(const Rect& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.w) && std::isfinite(value.h) &&
           value.w >= 0.0f && value.h >= 0.0f;
}

std::optional<Rect> ReadRect(const VariantObject& object,
                             const std::string_view key) {
    const auto* value = Property(object, key);
    if (!value) return std::nullopt;
    if (const auto* rect = value->TryGet<Rect>()) return *rect;
    const auto* nested = value->AsObject();
    if (!nested) return std::nullopt;
    const auto x = Number(*nested, "x"), y = Number(*nested, "y");
    const auto w = Number(*nested, "w"), h = Number(*nested, "h");
    if (!x || !y || !w || !h) return std::nullopt;
    return Rect{static_cast<float>(*x), static_cast<float>(*y),
                static_cast<float>(*w), static_cast<float>(*h)};
}

std::optional<Color> ReadColor(const VariantObject& object,
                               const std::string_view key) {
    const auto* value = Property(object, key);
    if (!value) return std::nullopt;
    if (const auto* color = value->TryGet<Color>()) return *color;
    const auto* nested = value->AsObject();
    if (!nested) return std::nullopt;
    const auto channel = [nested](const std::string_view name,
                                  const int fallback) -> std::optional<int> {
        const auto value = Number(*nested, name);
        if (!value) return fallback;
        if (!std::isfinite(*value) || *value < 0.0 || *value > 255.0)
            return std::nullopt;
        return static_cast<int>(*value);
    };
    const auto r = channel("r", 255), g = channel("g", 255);
    const auto b = channel("b", 255), a = channel("a", 255);
    if (!r || !g || !b || !a) return std::nullopt;
    return Color{static_cast<std::uint8_t>(*r), static_cast<std::uint8_t>(*g),
                 static_cast<std::uint8_t>(*b), static_cast<std::uint8_t>(*a)};
}

Variant RectState(const Rect& value) {
    return Variant(VariantObject{{"x", value.x}, {"y", value.y},
                                 {"w", value.w}, {"h", value.h}});
}

Variant ColorState(const Color value) {
    return Variant(VariantObject{{"r", static_cast<int>(value.r)},
                                 {"g", static_cast<int>(value.g)},
                                 {"b", static_cast<int>(value.b)},
                                 {"a", static_cast<int>(value.a)}});
}

std::string_view KindName(const ScriptRenderNode::Kind kind) {
    switch (kind) {
        case ScriptRenderNode::Kind::Image: return "image";
        case ScriptRenderNode::Kind::Rectangle: return "rect";
        case ScriptRenderNode::Kind::RoundedRectangle: return "rounded-rect";
        case ScriptRenderNode::Kind::Text: return "text";
    }
    return "image";
}

}  // namespace

std::optional<RenderPhase> ParseRenderPhase(const std::string_view value) {
    if (value == "before-stage") return RenderPhase::BeforeStage;
    if (value == "after-stage") return RenderPhase::AfterStage;
    if (value == "before-ui") return RenderPhase::BeforeUI;
    if (value == "after-ui") return RenderPhase::AfterUI;
    return std::nullopt;
}

std::string_view RenderPhaseName(const RenderPhase value) {
    switch (value) {
        case RenderPhase::BeforeStage: return "before-stage";
        case RenderPhase::AfterStage: return "after-stage";
        case RenderPhase::BeforeUI: return "before-ui";
        case RenderPhase::AfterUI: return "after-ui";
    }
    return "after-stage";
}

Status ScriptRenderTree::Upsert(ScriptRenderNode node) {
    if (node.id.empty() || node.id.size() > 256)
        return Status::Fail(RenderDiagnostic(
            "PXJS7701", "Retained render node id is invalid", node.id));
    if (m_nodes.size() >= kMaximumNodes && !m_nodes.contains(node.id))
        return Status::Fail(RenderDiagnostic(
            "PXJS7702", "Retained render node limit was exceeded"));
    if (!FiniteRect(node.bounds) ||
        (node.source && !FiniteRect(*node.source)) ||
        (node.clip && !FiniteRect(*node.clip)) ||
        !std::isfinite(node.translateX) || !std::isfinite(node.translateY) ||
        !std::isfinite(node.scaleX) || !std::isfinite(node.scaleY) ||
        !std::isfinite(node.rotationDegrees) || !std::isfinite(node.pivotX) ||
        !std::isfinite(node.pivotY) || !std::isfinite(node.radius))
        return Status::Fail(RenderDiagnostic(
            "PXJS7703", "Retained render node contains invalid geometry",
            node.id));
    if (node.kind == ScriptRenderNode::Kind::Image && node.path.empty())
        return Status::Fail(RenderDiagnostic(
            "PXJS7704", "Image render nodes require a resource path", node.id));
    if (node.kind == ScriptRenderNode::Kind::Text &&
        (node.font.empty() || node.fontSize <= 0 || node.fontSize > 512))
        return Status::Fail(RenderDiagnostic(
            "PXJS7705", "Text render node font is invalid", node.id));
    m_nodes.insert_or_assign(node.id, std::move(node));
    return Status::Ok();
}

bool ScriptRenderTree::Remove(const std::string_view id) {
    return m_nodes.erase(std::string(id)) != 0;
}

void ScriptRenderTree::Clear() { m_nodes.clear(); }

void ScriptRenderTree::Render(graphics::Renderer2D& renderer,
                              const RenderPhase phase,
                              const std::string_view currentRoute,
                              const std::string_view currentModalRoute) const {
    std::vector<const ScriptRenderNode*> ordered;
    ordered.reserve(m_nodes.size());
    for (const auto& [_, node] : m_nodes) {
        if (!node.visible || node.phase != phase ||
            (!node.route.empty() && node.route != currentRoute &&
             node.route != currentModalRoute))
            continue;
        ordered.push_back(&node);
    }
    std::ranges::sort(ordered, [](const auto* left, const auto* right) {
        return std::tie(left->order, left->id) <
               std::tie(right->order, right->id);
    });
    for (const auto* node : ordered) {
        if (node->clip) renderer.PushClip(*node->clip);
        const float radians = node->rotationDegrees *
                              3.14159265358979323846f / 180.0f;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        graphics::AffineTransform2D transform{
            .a = cosine * node->scaleX,
            .b = sine * node->scaleX,
            .c = -sine * node->scaleY,
            .d = cosine * node->scaleY};
        const float pivotX = node->bounds.x + node->pivotX;
        const float pivotY = node->bounds.y + node->pivotY;
        transform.tx = node->translateX + pivotX -
                       transform.a * pivotX - transform.c * pivotY;
        transform.ty = node->translateY + pivotY -
                       transform.b * pivotX - transform.d * pivotY;
        renderer.PushAffineTransform(transform, node->color);
        const Rect bounds = node->bounds;
        switch (node->kind) {
            case ScriptRenderNode::Kind::Image:
                if (node->source) {
                    renderer.DrawImageRegion(node->path, *node->source,
                                             bounds, 255);
                } else {
                    renderer.DrawImage(node->path, bounds, 255);
                }
                break;
            case ScriptRenderNode::Kind::Rectangle:
                renderer.DrawRect(bounds, {255, 255, 255, 255});
                break;
            case ScriptRenderNode::Kind::RoundedRectangle:
                renderer.DrawRoundedRect(bounds, node->radius,
                                         {255, 255, 255, 255});
                break;
            case ScriptRenderNode::Kind::Text:
                renderer.DrawText(node->text, bounds.x, bounds.y, node->font,
                                  node->fontSize, {255, 255, 255, 255});
                break;
        }
        renderer.PopTransform();
        if (node->clip) renderer.PopClip();
    }
}

Variant ScriptRenderTree::CaptureState() const {
    std::vector<const ScriptRenderNode*> ordered;
    ordered.reserve(m_nodes.size());
    for (const auto& [_, node] : m_nodes) ordered.push_back(&node);
    std::ranges::sort(ordered, {}, &ScriptRenderNode::id);
    VariantArray nodes;
    nodes.reserve(ordered.size());
    for (const auto* node : ordered) {
        VariantObject state{{"id", node->id},
                            {"kind", std::string(KindName(node->kind))},
                            {"phase", std::string(RenderPhaseName(node->phase))},
                            {"order", node->order},
                            {"visible", node->visible},
                            {"route", node->route},
                            {"path", node->path},
                            {"text", node->text},
                            {"font", node->font},
                            {"fontSize", node->fontSize},
                            {"bounds", RectState(node->bounds)},
                            {"color", ColorState(node->color)},
                            {"translateX", node->translateX},
                            {"translateY", node->translateY},
                            {"scaleX", node->scaleX},
                            {"scaleY", node->scaleY},
                            {"rotation", node->rotationDegrees},
                            {"pivotX", node->pivotX},
                            {"pivotY", node->pivotY},
                            {"radius", node->radius}};
        if (node->source) state.emplace("source", RectState(*node->source));
        if (node->clip) state.emplace("clip", RectState(*node->clip));
        nodes.emplace_back(std::move(state));
    }
    return Variant(VariantObject{{"nodes", Variant(std::move(nodes))}});
}

Result<ScriptRenderNode> ScriptRenderTree::ParseNode(
    const std::string_view id, const Variant& plan) {
    const auto* object = plan.AsObject();
    if (!object)
        return Result<ScriptRenderNode>::Failure(RenderDiagnostic(
            "PXJS7706", "Retained render node plan must be an object",
            std::string(id)));
    ScriptRenderNode node;
    node.id = std::string(id);
    const std::string kind = String(*object, "kind").value_or("image");
    if (kind == "image") node.kind = ScriptRenderNode::Kind::Image;
    else if (kind == "rect") node.kind = ScriptRenderNode::Kind::Rectangle;
    else if (kind == "rounded-rect")
        node.kind = ScriptRenderNode::Kind::RoundedRectangle;
    else if (kind == "text") node.kind = ScriptRenderNode::Kind::Text;
    else
        return Result<ScriptRenderNode>::Failure(RenderDiagnostic(
            "PXJS7707", "Unknown retained render node kind", kind));
    const std::string phase =
        String(*object, "phase").value_or("after-stage");
    const auto parsedPhase = ParseRenderPhase(phase);
    if (!parsedPhase)
        return Result<ScriptRenderNode>::Failure(RenderDiagnostic(
            "PXJS7708", "Unknown retained render phase", phase));
    node.phase = *parsedPhase;
    if (const auto value = Number(*object, "order"))
        node.order = static_cast<std::int64_t>(*value);
    if (const auto* value = Property(*object, "visible")) {
        if (const auto* visible = value->TryGet<bool>()) node.visible = *visible;
        else return Result<ScriptRenderNode>::Failure(RenderDiagnostic(
            "PXJS7709", "Render node visible must be boolean", node.id));
    }
    node.route = String(*object, "route").value_or(std::string{});
    node.path = String(*object, "path").value_or(std::string{});
    node.text = String(*object, "text").value_or(std::string{});
    node.font = String(*object, "font").value_or(std::string{});
    if (const auto value = Number(*object, "fontSize"))
        node.fontSize = static_cast<int>(*value);
    const auto bounds = ReadRect(*object, "bounds");
    if (!bounds)
        return Result<ScriptRenderNode>::Failure(RenderDiagnostic(
            "PXJS7710", "Render node bounds are required", node.id));
    node.bounds = *bounds;
    if (Property(*object, "source")) {
        node.source = ReadRect(*object, "source");
        if (!node.source)
            return Result<ScriptRenderNode>::Failure(RenderDiagnostic(
                "PXJS7711", "Render node source region is invalid", node.id));
    }
    if (Property(*object, "clip")) {
        node.clip = ReadRect(*object, "clip");
        if (!node.clip)
            return Result<ScriptRenderNode>::Failure(RenderDiagnostic(
                "PXJS7712", "Render node clip is invalid", node.id));
    }
    if (Property(*object, "color")) {
        const auto color = ReadColor(*object, "color");
        if (!color)
            return Result<ScriptRenderNode>::Failure(RenderDiagnostic(
                "PXJS7713", "Render node color is invalid", node.id));
        node.color = *color;
    }
    const auto assign = [object](const std::string_view name, float& target) {
        if (const auto value = Number(*object, name))
            target = static_cast<float>(*value);
    };
    assign("translateX", node.translateX);
    assign("translateY", node.translateY);
    assign("scaleX", node.scaleX);
    assign("scaleY", node.scaleY);
    assign("rotation", node.rotationDegrees);
    assign("pivotX", node.pivotX);
    assign("pivotY", node.pivotY);
    assign("radius", node.radius);
    ScriptRenderTree validation;
    if (const Status status = validation.Upsert(node); !status)
        return Result<ScriptRenderNode>::Failure(status.Diagnostics());
    return Result<ScriptRenderNode>::Success(std::move(node));
}

Status ScriptRenderTree::RestoreState(const Variant& state) {
    const auto* object = state.AsObject();
    const auto* nodesValue = object ? Property(*object, "nodes") : nullptr;
    const auto* nodes = nodesValue ? nodesValue->AsArray() : nullptr;
    if (!nodes)
        return Status::Fail(RenderDiagnostic(
            "PXJS7714", "Retained render tree checkpoint is invalid"));
    ScriptRenderTree candidate;
    for (const auto& value : *nodes) {
        const auto* nodeObject = value.AsObject();
        const auto id = nodeObject ? String(*nodeObject, "id") : std::nullopt;
        if (!id)
            return Status::Fail(RenderDiagnostic(
                "PXJS7715", "Retained render node checkpoint lacks an id"));
        auto parsed = ParseNode(*id, value);
        if (!parsed) return Status::Fail(parsed.Diagnostics());
        if (candidate.m_nodes.contains(*id))
            return Status::Fail(RenderDiagnostic(
                "PXJS7716", "Retained render checkpoint contains duplicate ids",
                *id));
        if (const Status added = candidate.Upsert(parsed.TakeValue()); !added)
            return added;
    }
    m_nodes = std::move(candidate.m_nodes);
    return Status::Ok();
}

}  // namespace px::script
