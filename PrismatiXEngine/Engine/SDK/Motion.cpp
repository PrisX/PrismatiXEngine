#include "Engine/SDK/Motion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace px::sdk {
namespace {
using Json = nlohmann::json;

bool OnlyFields(const Json& object, std::initializer_list<std::string_view> fields) {
    if (!object.is_object()) return false;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(fields.begin(), fields.end(), std::string_view(it.key())) == fields.end())
            return false;
    }
    return true;
}

bool Identifier(std::string_view text) {
    if (text.empty() || text.size() > 256) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || (i > 0 && (c == '.' || c == '_' || c == '-'));
        if (!valid) return false;
    }
    return true;
}

bool SafePath(std::string_view text) {
    if (text.empty() || text.size() > 4096 || text.front() == '/' ||
        text.find(':') != text.npos || text.find('\\') != text.npos) return false;
    for (const unsigned char character : text)
        if (character < 0x20) return false;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find('/', begin);
        const auto component = text.substr(begin, end == text.npos ? text.size() - begin : end - begin);
        if (component.empty() || component == "." || component == "..") return false;
        if (end == text.npos) break;
        begin = end + 1;
    }
    return true;
}

bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) return false;
    text.remove_prefix(text.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const char left = text[i], right = suffix[i];
        const char folded = left >= 'A' && left <= 'Z' ? static_cast<char>(left + 32) : left;
        if (folded != right) return false;
    }
    return true;
}

std::optional<double> Number(const Json& object, const char* key,
                             double fallback, double minimum, double maximum,
                             bool required = false) {
    const auto it = object.find(key);
    if (it == object.end()) return required ? std::nullopt : std::optional<double>(fallback);
    if (!it->is_number()) return std::nullopt;
    const double value = it->get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum) return std::nullopt;
    return value;
}

std::optional<bool> Boolean(const Json& object, const char* key, bool fallback,
                            bool required = false) {
    const auto it = object.find(key);
    if (it == object.end()) return required ? std::nullopt : std::optional<bool>(fallback);
    return it->is_boolean() ? std::optional<bool>(it->get<bool>()) : std::nullopt;
}

std::optional<MotionProperty> Property(std::string_view name) {
    if (name == "x") return MotionProperty::X;
    if (name == "y") return MotionProperty::Y;
    if (name == "scaleX") return MotionProperty::ScaleX;
    if (name == "scaleY") return MotionProperty::ScaleY;
    if (name == "rotation") return MotionProperty::Rotation;
    if (name == "opacity") return MotionProperty::Opacity;
    if (name == "visible") return MotionProperty::Visible;
    return std::nullopt;
}

std::optional<MotionCurve> Curve(std::string_view name) {
    if (name == "step") return MotionCurve::Step;
    if (name == "linear") return MotionCurve::Linear;
    if (name == "easeIn") return MotionCurve::EaseIn;
    if (name == "easeOut") return MotionCurve::EaseOut;
    if (name == "easeInOut") return MotionCurve::EaseInOut;
    return std::nullopt;
}

std::pair<double, double> PropertyRange(MotionProperty property) {
    switch (property) {
        case MotionProperty::X:
        case MotionProperty::Y:
        case MotionProperty::Rotation: return {-1'000'000, 1'000'000};
        case MotionProperty::ScaleX:
        case MotionProperty::ScaleY: return {-1000, 1000};
        case MotionProperty::Opacity: return {0, 1};
        case MotionProperty::Visible: return {0, 1};
    }
    return {0, 0};
}

MotionAffine Multiply(const MotionAffine& parent, const MotionAffine& child) {
    return {
        parent.a * child.a + parent.c * child.b,
        parent.b * child.a + parent.d * child.b,
        parent.a * child.c + parent.c * child.d,
        parent.b * child.c + parent.d * child.d,
        parent.a * child.tx + parent.c * child.ty + parent.tx,
        parent.b * child.tx + parent.d * child.ty + parent.ty,
    };
}

double Ease(MotionCurve curve, double value) {
    switch (curve) {
        case MotionCurve::Step: return 0;
        case MotionCurve::Linear: return value;
        case MotionCurve::EaseIn: return value * value;
        case MotionCurve::EaseOut: return 1 - (1 - value) * (1 - value);
        case MotionCurve::EaseInOut:
            return value < 0.5 ? 2 * value * value : 1 - 2 * (1 - value) * (1 - value);
    }
    return value;
}

std::optional<double> SampleNumber(const MotionTrack& track, double time) {
    const auto& keys = track.keys;
    const auto right = std::upper_bound(keys.begin(), keys.end(), time,
        [](double at, const MotionKey& key) { return at < key.time; });
    if (right == keys.begin()) return std::nullopt;
    const auto& left = *(right - 1);
    if (right == keys.end() || left.curve == MotionCurve::Step) return left.value;
    const double fraction = (time - left.time) / (right->time - left.time);
    return left.value + (right->value - left.value) * Ease(left.curve, fraction);
}
}  // namespace

MotionParseResult ParseMotion(const std::string_view text, std::string sourcePath) {
    MotionParseResult result;
    const auto fail = [&](std::string code, std::string message) {
        result.diagnostics.push_back({std::move(code), std::move(message), sourcePath});
        return result;
    };
    if (text.size() > 4 * 1024 * 1024 || (!text.empty() &&
        static_cast<unsigned char>(text.front()) == 0xEF))
        return fail("PXMOTION1000", "Motion document exceeds 4 MiB or has a UTF-8 BOM");
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !OnlyFields(root, {"format", "schemaRevision", "id", "canvas",
        "duration", "loop", "scaleMode", "layers", "tracks"}))
        return fail("PXMOTION1001", "Motion document is invalid JSON or contains unknown fields");
    if (!root.contains("format") || !root["format"].is_string() ||
        root["format"].get_ref<const std::string&>() != "PrismatiXMotion" ||
        !root.contains("schemaRevision") || !root["schemaRevision"].is_number_integer() ||
        root["schemaRevision"] != 1 || !root.contains("id") ||
        !root["id"].is_string() || !Identifier(root["id"].get_ref<const std::string&>()))
        return fail("PXMOTION1002", "Motion format, revision, or id is invalid");
    const auto canvas = root.find("canvas");
    if (canvas == root.end() || !OnlyFields(*canvas, {"width", "height"}))
        return fail("PXMOTION1003", "Motion canvas must specify width and height");
    const auto canvasWidth = Number(*canvas, "width", 0, 0.000001, 16384, true);
    const auto canvasHeight = Number(*canvas, "height", 0, 0.000001, 16384, true);
    const auto duration = Number(root, "duration", 0, 0.000001, 86400, true);
    const auto loop = Boolean(root, "loop", false, true);
    if (!canvasWidth || !canvasHeight || !duration || !loop)
        return fail("PXMOTION1003", "Motion canvas, duration, or loop is invalid");
    MotionClip clip;
    clip.id = root["id"].get<std::string>();
    clip.canvasWidth = *canvasWidth;
    clip.canvasHeight = *canvasHeight;
    clip.duration = *duration;
    clip.loop = *loop;
    if (const auto mode = root.find("scaleMode"); mode != root.end()) {
        if (!mode->is_string()) return fail("PXMOTION1004", "Motion scaleMode is invalid");
        const auto value = mode->get<std::string>();
        if (value == "fit") clip.scaleMode = MotionScaleMode::Fit;
        else if (value == "fill") clip.scaleMode = MotionScaleMode::Fill;
        else if (value == "stretch") clip.scaleMode = MotionScaleMode::Stretch;
        else return fail("PXMOTION1004", "Motion scaleMode is invalid");
    }
    const auto layers = root.find("layers");
    if (layers == root.end() || !layers->is_array() || layers->empty() || layers->size() > 4096)
        return fail("PXMOTION1005", "Motion layers must contain 1 to 4096 entries");
    std::unordered_map<std::string, std::size_t> layerIds;
    std::vector<std::string> parents;
    clip.layers.reserve(layers->size()); parents.reserve(layers->size());
    for (const auto& entry : *layers) {
        if (!OnlyFields(entry, {"id", "parent", "image", "z", "width", "height",
            "anchorX", "anchorY", "x", "y", "scaleX", "scaleY", "rotation",
            "opacity", "visible"}) || !entry.contains("id") || !entry["id"].is_string() ||
            !entry.contains("image") || !entry["image"].is_string())
            return fail("PXMOTION1006", "Motion layer has invalid or unknown fields");
        MotionLayer layer;
        layer.id = entry["id"].get<std::string>();
        layer.image = entry["image"].get<std::string>();
        if (!Identifier(layer.id) || !SafePath(layer.image) ||
            EndsWithIgnoreCase(layer.image, ".pxmotion") ||
            EndsWithIgnoreCase(layer.image, ".pxportrait") ||
            !layerIds.emplace(layer.id, clip.layers.size()).second)
            return fail("PXMOTION1007", "Motion layer id or image path is invalid or duplicated");
        if (const auto parent = entry.find("parent"); parent != entry.end()) {
            if (!parent->is_string() || !Identifier(parent->get_ref<const std::string&>()))
                return fail("PXMOTION1008", "Motion layer parent id is invalid");
            parents.push_back(parent->get<std::string>());
        } else parents.emplace_back();
        const auto z = Number(entry, "z", 0, -100000, 100000);
        const auto width = Number(entry, "width", 0, 0.000001, 16384, true);
        const auto height = Number(entry, "height", 0, 0.000001, 16384, true);
        const auto anchorX = Number(entry, "anchorX", 0, 0, 1);
        const auto anchorY = Number(entry, "anchorY", 0, 0, 1);
        const auto x = Number(entry, "x", 0, -1'000'000, 1'000'000);
        const auto y = Number(entry, "y", 0, -1'000'000, 1'000'000);
        const auto scaleX = Number(entry, "scaleX", 1, -1000, 1000);
        const auto scaleY = Number(entry, "scaleY", 1, -1000, 1000);
        const auto rotation = Number(entry, "rotation", 0, -1'000'000, 1'000'000);
        const auto opacity = Number(entry, "opacity", 1, 0, 1);
        const auto visible = Boolean(entry, "visible", true);
        if (!z || std::floor(*z) != *z || !width || !height || !anchorX || !anchorY ||
            !x || !y || !scaleX || !scaleY || !rotation || !opacity || !visible)
            return fail("PXMOTION1009", "Motion layer geometry or visibility is invalid");
        layer.z = static_cast<int>(*z);
        layer.width = *width; layer.height = *height;
        layer.anchorX = *anchorX; layer.anchorY = *anchorY;
        layer.x = *x; layer.y = *y; layer.scaleX = *scaleX; layer.scaleY = *scaleY;
        layer.rotation = *rotation; layer.opacity = *opacity; layer.visible = *visible;
        clip.layers.push_back(std::move(layer));
    }
    std::vector<std::vector<std::size_t>> children(clip.layers.size());
    std::vector<std::size_t> roots;
    for (std::size_t i = 0; i < parents.size(); ++i) {
        if (parents[i].empty()) { roots.push_back(i); continue; }
        const auto it = layerIds.find(parents[i]);
        if (it == layerIds.end()) return fail("PXMOTION1010", "Motion layer parent does not exist");
        clip.layers[i].parent = it->second;
        children[it->second].push_back(i);
    }
    std::deque<std::size_t> queue(roots.begin(), roots.end());
    while (!queue.empty()) {
        const auto index = queue.front(); queue.pop_front();
        clip.hierarchyOrder.push_back(index);
        for (const auto child : children[index]) queue.push_back(child);
    }
    if (clip.hierarchyOrder.size() != clip.layers.size())
        return fail("PXMOTION1011", "Motion layer hierarchy contains a cycle");
    clip.drawOrder.resize(clip.layers.size());
    std::iota(clip.drawOrder.begin(), clip.drawOrder.end(), std::size_t{0});
    std::stable_sort(clip.drawOrder.begin(), clip.drawOrder.end(), [&](std::size_t left, std::size_t right) {
        return clip.layers[left].z < clip.layers[right].z;
    });
    const auto tracks = root.find("tracks");
    if (tracks == root.end() || !tracks->is_array() || tracks->size() > 65536)
        return fail("PXMOTION1012", "Motion tracks must be an array within the contract limit");
    std::unordered_set<std::string> targets;
    std::size_t totalKeys = 0;
    clip.tracks.reserve(tracks->size());
    for (const auto& entry : *tracks) {
        if (!OnlyFields(entry, {"layer", "property", "keys"}) ||
            !entry.contains("layer") || !entry["layer"].is_string() ||
            !entry.contains("property") || !entry["property"].is_string() ||
            !entry.contains("keys") || !entry["keys"].is_array() ||
            entry["keys"].empty() || entry["keys"].size() > 65536)
            return fail("PXMOTION1013", "Motion track shape is invalid");
        const auto layerName = entry["layer"].get<std::string>();
        const auto propertyName = entry["property"].get<std::string>();
        const auto layer = layerIds.find(layerName);
        const auto property = Property(propertyName);
        if (layer == layerIds.end() || !property ||
            !targets.insert(layerName + "." + propertyName).second)
            return fail("PXMOTION1014", "Motion track target is unknown or duplicated");
        totalKeys += entry["keys"].size();
        if (totalKeys > 1'000'000)
            return fail("PXMOTION1015", "Motion clip exceeds the one-million-key limit");
        MotionTrack track;
        track.layer = layer->second;
        track.property = *property;
        track.keys.reserve(entry["keys"].size());
        double previous = -1;
        for (const auto& key : entry["keys"]) {
            if (!OnlyFields(key, {"time", "value", "curve"}) ||
                !key.contains("curve") || !key["curve"].is_string())
                return fail("PXMOTION1016", "Motion key shape is invalid");
            const auto time = Number(key, "time", 0, 0, clip.duration, true);
            const auto curve = Curve(key["curve"].get<std::string>());
            if (!time || *time <= previous || !curve || !key.contains("value"))
                return fail("PXMOTION1017", "Motion key time or curve is invalid");
            MotionKey parsed;
            parsed.time = *time;
            parsed.curve = *curve;
            if (*property == MotionProperty::Visible) {
                if (!key["value"].is_boolean() || *curve != MotionCurve::Step)
                    return fail("PXMOTION1018", "Visible motion keys require boolean step values");
                parsed.visible = key["value"].get<bool>();
            } else {
                const auto [minimum, maximum] = PropertyRange(*property);
                const auto value = Number(key, "value", 0, minimum, maximum, true);
                if (!value) return fail("PXMOTION1019", "Motion key value is outside its property range");
                parsed.value = *value;
            }
            track.keys.push_back(parsed);
            previous = *time;
        }
        clip.tracks.push_back(std::move(track));
    }
    result.clip = std::move(clip);
    return result;
}

std::vector<std::string> MotionDependencies(const MotionClip& clip) {
    std::vector<std::string> dependencies;
    std::unordered_set<std::string> seen;
    for (const auto& layer : clip.layers)
        if (seen.insert(layer.image).second) dependencies.push_back(layer.image);
    return dependencies;
}

MotionSampler::MotionSampler(const MotionClip& clip) : m_clip(clip) {
    m_states.resize(clip.layers.size());
    m_world.resize(clip.layers.size());
    m_pose.resize(clip.layers.size());
    Seek(0);
}

void MotionSampler::Seek(double timeSeconds) {
    const double finite = std::isfinite(timeSeconds) ? std::max(0.0, timeSeconds) : 0.0;
    m_position = m_clip.loop ? std::fmod(finite, m_clip.duration) :
        std::min(finite, m_clip.duration);
    for (std::size_t i = 0; i < m_clip.layers.size(); ++i) {
        const auto& layer = m_clip.layers[i];
        m_states[i] = {layer.x, layer.y, layer.scaleX, layer.scaleY,
                       layer.rotation, layer.opacity, layer.visible};
    }
    for (const auto& track : m_clip.tracks) {
        auto& state = m_states[track.layer];
        if (track.property == MotionProperty::Visible) {
            const auto right = std::upper_bound(track.keys.begin(), track.keys.end(), m_position,
                [](double at, const MotionKey& key) { return at < key.time; });
            if (right != track.keys.begin()) state.visible = (right - 1)->visible;
            continue;
        }
        const auto sampled = SampleNumber(track, m_position);
        if (!sampled) continue;
        const double value = *sampled;
        switch (track.property) {
            case MotionProperty::X: state.x = value; break;
            case MotionProperty::Y: state.y = value; break;
            case MotionProperty::ScaleX: state.scaleX = value; break;
            case MotionProperty::ScaleY: state.scaleY = value; break;
            case MotionProperty::Rotation: state.rotation = value; break;
            case MotionProperty::Opacity: state.opacity = value; break;
            case MotionProperty::Visible: break;
        }
    }
    constexpr double radiansPerDegree = 3.14159265358979323846 / 180.0;
    for (const auto index : m_clip.hierarchyOrder) {
        const auto& layer = m_clip.layers[index];
        const auto& state = m_states[index];
        const double radians = state.rotation * radiansPerDegree;
        const double cosine = std::cos(radians), sine = std::sin(radians);
        MotionAffine local;
        local.a = cosine * state.scaleX;
        local.b = sine * state.scaleX;
        local.c = -sine * state.scaleY;
        local.d = cosine * state.scaleY;
        local.tx = state.x - local.a * layer.anchorX * layer.width -
            local.c * layer.anchorY * layer.height;
        local.ty = state.y - local.b * layer.anchorX * layer.width -
            local.d * layer.anchorY * layer.height;
        auto& world = m_world[index];
        world.layer = index;
        world.transform = layer.parent ? Multiply(m_world[*layer.parent].transform, local) : local;
        world.opacity = state.opacity * (layer.parent ? m_world[*layer.parent].opacity : 1);
        world.visible = state.visible && (!layer.parent || m_world[*layer.parent].visible);
    }
    for (std::size_t i = 0; i < m_pose.size(); ++i) m_pose[i] = m_world[m_clip.drawOrder[i]];
}

}  // namespace px::sdk
