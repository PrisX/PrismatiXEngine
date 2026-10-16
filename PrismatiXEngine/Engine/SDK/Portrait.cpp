#include "Engine/SDK/Portrait.h"

#include <cmath>
#include <nlohmann/json.hpp>

namespace px::sdk {
namespace {
using Json = nlohmann::json;

bool Number(const Json& object, const char* name, float& out,
            const double minimum, const double maximum, const bool required) {
    const auto field = object.find(name);
    if (field == object.end()) return !required;
    if (!field->is_number()) return false;
    const double value = field->get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum) return false;
    out = static_cast<float>(value);
    return true;
}

bool SafePath(const std::string_view path) {
    if (path.empty() || path.size() > 4096 || path.front() == '/' ||
        path.find('\\') != path.npos || path.find(':') != path.npos)
        return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == path.npos ? path.npos : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == path.npos) break;
        start = end + 1;
    }
    return true;
}
bool Frames(const Json& layer, const char* name, const PortraitLayer& owner,
            std::vector<PortraitFrame>& out) {
    const auto field = layer.find(name);
    if (field == layer.end()) return true;
    if (!field->is_array() || field->empty() || field->size() > 16 ||
        owner.role != PortraitLayer::Role::Expression)
        return false;
    out.reserve(field->size());
    for (const auto& value : *field) {
        if (!value.is_object()) return false;
        const auto path = value.find("path");
        if (path == value.end() || !path->is_string() ||
            !SafePath(path->get_ref<const std::string&>()) ||
            path->get_ref<const std::string&>().ends_with(".pxportrait"))
            return false;
        PortraitFrame frame{path->get<std::string>(), owner.x, owner.y, 0.0f, 0.0f};
        if (!Number(value, "x", frame.x, -1000000, 1000000, false) ||
            !Number(value, "y", frame.y, -1000000, 1000000, false) ||
            !Number(value, "width", frame.width, 0, 16384, true) ||
            !Number(value, "height", frame.height, 0, 16384, true) ||
            frame.width <= 0 || frame.height <= 0)
            return false;
        out.push_back(std::move(frame));
    }
    return true;
}
}  // namespace

std::optional<LayeredPortrait> ParseLayeredPortrait(const std::string_view source) {
    if (source.size() > 64 * 1024 * 1024) return std::nullopt;
    const Json document = Json::parse(source, nullptr, false);
    if (!document.is_object()) return std::nullopt;
    const auto format = document.find("format");
    if (format == document.end() || !format->is_string() ||
        *format != "PrismatiXLayeredPortrait") return std::nullopt;
    const auto revision = document.find("schemaRevision");
    if (revision == document.end() || !revision->is_number_integer()) return std::nullopt;
    if (*revision != 1) return std::nullopt;
    LayeredPortrait portrait;
    portrait.schemaRevision = 1;
    if (!Number(document, "width", portrait.width, 0, 16384, true) ||
        !Number(document, "height", portrait.height, 0, 16384, true) ||
        portrait.width <= 0 || portrait.height <= 0)
        return std::nullopt;
    const auto layers = document.find("layers");
    if (layers == document.end() || !layers->is_array() || layers->empty() ||
        layers->size() > 10000) return std::nullopt;
    portrait.layers.reserve(layers->size());
    for (const auto& value : *layers) {
        if (!value.is_object()) return std::nullopt;
        const auto path = value.find("path");
        if (path == value.end() || !path->is_string() ||
            !SafePath(path->get_ref<const std::string&>()) ||
            path->get_ref<const std::string&>().ends_with(".pxportrait"))
            return std::nullopt;
        PortraitLayer layer;
        layer.path = path->get<std::string>();
        if (!Number(value, "x", layer.x, -1000000, 1000000, false) ||
            !Number(value, "y", layer.y, -1000000, 1000000, false) ||
            !Number(value, "width", layer.width, 0, 16384, true) ||
            !Number(value, "height", layer.height, 0, 16384, true) ||
            layer.width <= 0 || layer.height <= 0)
            return std::nullopt;
        const auto opacity = value.find("opacity");
        if (opacity != value.end()) {
            if (!opacity->is_number_integer()) return std::nullopt;
            if (*opacity < 0 || *opacity > 255) return std::nullopt;
            const int alpha = opacity->get<int>();
            layer.opacity = static_cast<std::uint8_t>(alpha);
        }
        const auto role = value.find("role");
        if (role == value.end() || !role->is_string()) return std::nullopt;
        const auto& name = role->get_ref<const std::string&>();
        if (name == "base") layer.role = PortraitLayer::Role::Base;
        else if (name == "expression") layer.role = PortraitLayer::Role::Expression;
        else return std::nullopt;
        if (!Frames(value, "blink", layer, layer.blink) ||
            !Frames(value, "mouth", layer, layer.mouth))
            return std::nullopt;
        portrait.layers.push_back(std::move(layer));
    }
    return portrait;
}

}  // namespace px::sdk
