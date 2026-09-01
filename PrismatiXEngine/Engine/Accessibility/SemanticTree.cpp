#include "Engine/Accessibility/SemanticTree.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace px::accessibility {
namespace {

Rect Intersect(const Rect left, const Rect right) {
    const float x = std::max(left.x, right.x);
    const float y = std::max(left.y, right.y);
    const float edgeX = std::min(left.x + left.w, right.x + right.w);
    const float edgeY = std::min(left.y + left.h, right.y + right.h);
    return {x, y, std::max(0.0f, edgeX - x), std::max(0.0f, edgeY - y)};
}

SemanticNode BuildNode(const ui::Control& control,
                       const std::optional<Rect>& ancestorClip) {
    SemanticNode output;
    output.semantics = control.DescribeAccessibility();
    if (ancestorClip) {
        output.semantics.bounds = Intersect(output.semantics.bounds, *ancestorClip);
        if (output.semantics.bounds.w <= 0.0f || output.semantics.bounds.h <= 0.0f)
            output.semantics.hidden = true;
    }
    std::optional<Rect> childClip = ancestorClip;
    if (control.ClipContent())
        childClip = childClip ? Intersect(*childClip, control.LayoutRect())
                              : std::optional<Rect>(control.LayoutRect());
    for (const auto& child : control.Children()) {
        if (const auto* childControl = dynamic_cast<const ui::Control*>(child.get()))
            output.children.push_back(BuildNode(*childControl, childClip));
    }
    return output;
}

nlohmann::ordered_json JsonNode(const SemanticNode& node) {
    const auto& value = node.semantics;
    nlohmann::ordered_json result{
        {"id", value.id.ToString()},
        {"role", value.role},
        {"label", value.label},
        {"value", value.value},
        {"description", value.description},
        {"states", value.states},
        {"actions", value.actions},
        {"bounds", {{"x", value.bounds.x}, {"y", value.bounds.y},
                    {"width", value.bounds.w}, {"height", value.bounds.h}}},
        {"range", value.hasRange
            ? nlohmann::ordered_json{{"minimum", value.minimum},
                                     {"maximum", value.maximum},
                                     {"step", value.step}}
            : nlohmann::ordered_json(nullptr)},
        {"readOnly", value.readOnly},
        {"focusOrder", value.focusOrder},
        {"focusable", value.focusable},
        {"hidden", value.hidden},
    };
    if (value.text) {
        nlohmann::ordered_json clusters = nlohmann::ordered_json::array();
        for (const auto& cluster : value.text->layout.Clusters()) {
            clusters.push_back({
                {"byteStart", cluster.byteStart},
                {"byteLength", cluster.byteLength},
                {"line", cluster.line},
                {"bounds", {{"x", cluster.bounds.x},
                            {"y", cluster.bounds.y},
                            {"width", cluster.bounds.w},
                            {"height", cluster.bounds.h}}},
                {"rotated", cluster.rotated},
                {"rightToLeft", cluster.rightToLeft},
            });
        }
        result["text"] = {
            {"content", value.text->layout.Text()},
            {"locale", value.text->layout.Locale()},
            {"origin", {{"x", value.text->origin.x},
                        {"y", value.text->origin.y}}},
            {"caretByteOffset", value.text->caretByteOffset},
            {"selectionStartByteOffset", value.text->selectionStartByteOffset},
            {"selectionEndByteOffset", value.text->selectionEndByteOffset},
            {"editable", value.text->editable},
            {"multiline", value.text->multiline},
            {"clusters", std::move(clusters)},
        };
    } else {
        result["text"] = nullptr;
    }
    result["children"] = nlohmann::ordered_json::array();
    for (const auto& child : node.children) result["children"].push_back(JsonNode(child));
    return result;
}

}  // namespace

SemanticTree SemanticTreeBuilder::Build(const ui::Control& root,
                                        const std::uint64_t revision) {
    return {.revision = revision, .root = BuildNode(root, std::nullopt)};
}

void MockSemanticAdapter::Publish(const SemanticTree& tree) {
    m_tree = tree;
    nlohmann::ordered_json json{{"revision", tree.revision},
                                {"root", JsonNode(tree.root)}};
    m_json = json.dump();
}

}  // namespace px::accessibility
