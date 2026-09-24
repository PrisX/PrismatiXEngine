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
    if (name == "frame") return MotionProperty::Frame;
    return std::nullopt;
}

std::optional<MotionCurve> Curve(std::string_view name) {
    if (name == "step") return MotionCurve::Step;
    if (name == "linear") return MotionCurve::Linear;
    if (name == "easeIn") return MotionCurve::EaseIn;
    if (name == "easeOut") return MotionCurve::EaseOut;
    if (name == "easeInOut") return MotionCurve::EaseInOut;
    if (name == "cubicBezier") return MotionCurve::CubicBezier;
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
        case MotionProperty::Frame: return {0, 4095};
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

double Ease(const MotionKey& key, double value) {
    switch (key.curve) {
        case MotionCurve::Step: return 0;
        case MotionCurve::Linear: return value;
        case MotionCurve::EaseIn: return value * value;
        case MotionCurve::EaseOut: return 1 - (1 - value) * (1 - value);
        case MotionCurve::EaseInOut:
            return value < 0.5 ? 2 * value * value : 1 - 2 * (1 - value) * (1 - value);
        case MotionCurve::CubicBezier: {
            if (value <= 0) return 0;
            if (value >= 1) return 1;
            const auto sample = [](double first, double second, double t) {
                const double inverse = 1 - t;
                return 3 * inverse * inverse * t * first +
                    3 * inverse * t * t * second + t * t * t;
            };
            double low = 0, high = 1;
            for (int iteration = 0; iteration < 24; ++iteration) {
                const double middle = (low + high) * 0.5;
                if (sample(key.bezierX1, key.bezierX2, middle) < value)
                    low = middle;
                else high = middle;
            }
            return sample(key.bezierY1, key.bezierY2, (low + high) * 0.5);
        }
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
    return left.value + (right->value - left.value) * Ease(left, fraction);
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
        "duration", "loop", "scaleMode", "layers", "tracks", "markers"}))
        return fail("PXMOTION1001", "Motion document is invalid JSON or contains unknown fields");
    if (!root.contains("format") || !root["format"].is_string() ||
        root["format"].get_ref<const std::string&>() != "PrismatiXMotion" ||
        !root.contains("schemaRevision") || !root["schemaRevision"].is_number_integer() ||
        (root["schemaRevision"] != 1 && root["schemaRevision"] != 2 &&
         root["schemaRevision"] != 3 && root["schemaRevision"] != 4 &&
         root["schemaRevision"] != 5 && root["schemaRevision"] != 6 &&
         root["schemaRevision"] != 7) ||
        !root.contains("id") ||
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
    clip.schemaRevision = root["schemaRevision"].get<int>();
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
    std::vector<std::string> masks;
    clip.layers.reserve(layers->size()); parents.reserve(layers->size());
    masks.reserve(layers->size());
    for (const auto& entry : *layers) {
        if (!OnlyFields(entry, {"id", "kind", "parent", "mask", "image", "frames", "z", "blendMode", "width", "height",
            "anchorX", "anchorY", "x", "y", "scaleX", "scaleY", "rotation",
            "timeOffset", "timeScale", "opacity", "visible"}) ||
            !entry.contains("id") || !entry["id"].is_string())
            return fail("PXMOTION1006", "Motion layer has invalid or unknown fields");
        MotionLayer layer;
        layer.id = entry["id"].get<std::string>();
        if (const auto kind = entry.find("kind"); kind != entry.end()) {
            if (clip.schemaRevision < 7 || !kind->is_string())
                return fail("PXMOTION1030", "Motion layer kind requires revision 7");
            const auto value = kind->get<std::string>();
            if (value == "group") layer.kind = MotionLayerKind::Group;
            else if (value != "image")
                return fail("PXMOTION1030", "Motion layer kind is invalid");
        }
        if (layer.kind == MotionLayerKind::Group) {
            if (entry.contains("image") || entry.contains("frames") ||
                entry.contains("mask") || entry.contains("blendMode"))
                return fail("PXMOTION1031", "Motion transform group cannot draw or mask images");
        } else {
            if (!entry.contains("image") || !entry["image"].is_string())
                return fail("PXMOTION1006", "Motion image layer requires an image path");
            layer.image = entry["image"].get<std::string>();
        }
        if (const auto blend = entry.find("blendMode"); blend != entry.end()) {
            if (clip.schemaRevision < 4 || !blend->is_string())
                return fail("PXMOTION1026", "Motion blend mode requires revision 4");
            const auto value = blend->get<std::string>();
            if (value == "normal") layer.blendMode = MotionBlendMode::Normal;
            else if (value == "additive") layer.blendMode = MotionBlendMode::Additive;
            else if (value == "multiply") layer.blendMode = MotionBlendMode::Multiply;
            else return fail("PXMOTION1026", "Motion blend mode is invalid");
        }
        if (!Identifier(layer.id) ||
            (layer.kind == MotionLayerKind::Image &&
                (!SafePath(layer.image) ||
                 EndsWithIgnoreCase(layer.image, ".pxmotion") ||
                 EndsWithIgnoreCase(layer.image, ".pxportrait"))) ||
            !layerIds.emplace(layer.id, clip.layers.size()).second)
            return fail("PXMOTION1007", "Motion layer id or image path is invalid or duplicated");
        if (const auto frames = entry.find("frames"); frames != entry.end()) {
            if (clip.schemaRevision < 2 || !frames->is_array() ||
                frames->empty() || frames->size() > 4096)
                return fail("PXMOTION1020", "Motion atlas frames require revision 2 and 1 to 4096 rectangles");
            layer.frames.reserve(frames->size());
            for (const auto& frame : *frames) {
                if (!OnlyFields(frame, {"x", "y", "width", "height"}) ||
                    frame.size() != 4)
                    return fail("PXMOTION1021", "Motion atlas frame rectangle is invalid");
                const auto x = Number(frame, "x", 0, 0, 16384, true);
                const auto y = Number(frame, "y", 0, 0, 16384, true);
                const auto width = Number(frame, "width", 0, 1, 16384, true);
                const auto height = Number(frame, "height", 0, 1, 16384, true);
                if (!x || !y || !width || !height ||
                    std::floor(*x) != *x || std::floor(*y) != *y ||
                    std::floor(*width) != *width || std::floor(*height) != *height ||
                    *x + *width > 16384 || *y + *height > 16384)
                    return fail("PXMOTION1021", "Motion atlas frame rectangle is invalid");
                layer.frames.push_back({static_cast<int>(*x), static_cast<int>(*y),
                    static_cast<int>(*width), static_cast<int>(*height)});
            }
        }
        if (const auto parent = entry.find("parent"); parent != entry.end()) {
            if (!parent->is_string() || !Identifier(parent->get_ref<const std::string&>()))
                return fail("PXMOTION1008", "Motion layer parent id is invalid");
            parents.push_back(parent->get<std::string>());
        } else parents.emplace_back();
        if (const auto mask = entry.find("mask"); mask != entry.end()) {
            if (clip.schemaRevision < 6 || !mask->is_string() ||
                !Identifier(mask->get_ref<const std::string&>()))
                return fail("PXMOTION1027", "Motion mask requires revision 6 and a valid layer id");
            masks.push_back(mask->get<std::string>());
        } else masks.emplace_back();
        const auto z = Number(entry, "z", 0, -100000, 100000);
        const bool imageLayer = layer.kind == MotionLayerKind::Image;
        const auto width = Number(entry, "width", 0, imageLayer ? 0.000001 : 0,
                                  16384, imageLayer);
        const auto height = Number(entry, "height", 0, imageLayer ? 0.000001 : 0,
                                   16384, imageLayer);
        const auto anchorX = Number(entry, "anchorX", 0, 0, 1);
        const auto anchorY = Number(entry, "anchorY", 0, 0, 1);
        const auto x = Number(entry, "x", 0, -1'000'000, 1'000'000);
        const auto y = Number(entry, "y", 0, -1'000'000, 1'000'000);
        const auto scaleX = Number(entry, "scaleX", 1, -1000, 1000);
        const auto scaleY = Number(entry, "scaleY", 1, -1000, 1000);
        const auto rotation = Number(entry, "rotation", 0, -1'000'000, 1'000'000);
        const auto timeOffset = Number(entry, "timeOffset", 0, -86400, 86400);
        const auto timeScale = Number(entry, "timeScale", 1, -1000, 1000);
        const auto opacity = Number(entry, "opacity", 1, 0, 1);
        const auto visible = Boolean(entry, "visible", true);
        if (!z || std::floor(*z) != *z || !width || !height || !anchorX || !anchorY ||
            !x || !y || !scaleX || !scaleY || !rotation || !timeOffset ||
            !timeScale || !opacity || !visible ||
            (clip.schemaRevision < 7 &&
                (entry.contains("timeOffset") || entry.contains("timeScale"))))
            return fail("PXMOTION1009", "Motion layer geometry or visibility is invalid");
        layer.z = static_cast<int>(*z);
        layer.width = *width; layer.height = *height;
        layer.anchorX = *anchorX; layer.anchorY = *anchorY;
        layer.x = *x; layer.y = *y; layer.scaleX = *scaleX; layer.scaleY = *scaleY;
        layer.rotation = *rotation; layer.opacity = *opacity; layer.visible = *visible;
        layer.timeOffset = *timeOffset; layer.timeScale = *timeScale;
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
    clip.maskSource.resize(clip.layers.size(), false);
    for (std::size_t i = 0; i < masks.size(); ++i) {
        if (masks[i].empty()) continue;
        const auto target = layerIds.find(masks[i]);
        if (target == layerIds.end() || target->second == i ||
            clip.layers[target->second].kind != MotionLayerKind::Image ||
            !masks[target->second].empty())
            return fail("PXMOTION1028", "Motion mask must name an unmasked, distinct layer");
        if (clip.layers[i].blendMode != MotionBlendMode::Normal ||
            clip.layers[target->second].blendMode != MotionBlendMode::Normal)
            return fail("PXMOTION1029", "Masked layers require normal blending");
        clip.layers[i].mask = target->second;
        clip.maskSource[target->second] = true;
    }
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
        if (*property == MotionProperty::Frame &&
            (clip.schemaRevision < 2 || clip.layers[layer->second].frames.empty()))
            return fail("PXMOTION1022", "Motion frame tracks require revision 2 atlas frames");
        totalKeys += entry["keys"].size();
        if (totalKeys > 1'000'000)
            return fail("PXMOTION1015", "Motion clip exceeds the one-million-key limit");
        MotionTrack track;
        track.layer = layer->second;
        track.property = *property;
        track.keys.reserve(entry["keys"].size());
        double previous = -1;
        for (const auto& key : entry["keys"]) {
            if (!OnlyFields(key, {"time", "value", "curve", "bezier"}) ||
                !key.contains("curve") || !key["curve"].is_string())
                return fail("PXMOTION1016", "Motion key shape is invalid");
            // Revision 7 keys use layer-local seconds. A faster nested clock
            // can reach key times beyond the enclosing clip duration.
            const auto time = Number(key, "time", 0, 0,
                clip.schemaRevision >= 7 ? 86400.0 : clip.duration, true);
            const auto curve = Curve(key["curve"].get<std::string>());
            if (!time || *time <= previous || !curve || !key.contains("value"))
                return fail("PXMOTION1017", "Motion key time or curve is invalid");
            MotionKey parsed;
            parsed.time = *time;
            parsed.curve = *curve;
            const auto bezier = key.find("bezier");
            if (*curve == MotionCurve::CubicBezier) {
                if (clip.schemaRevision < 5 || bezier == key.end() ||
                    !OnlyFields(*bezier, {"x1", "y1", "x2", "y2"}) ||
                    bezier->size() != 4)
                    return fail("PXMOTION1027", "Cubic Bezier keys require revision 5 and four control coordinates");
                const auto x1 = Number(*bezier, "x1", 0, 0, 1, true);
                const auto y1 = Number(*bezier, "y1", 0, -8, 8, true);
                const auto x2 = Number(*bezier, "x2", 0, 0, 1, true);
                const auto y2 = Number(*bezier, "y2", 0, -8, 8, true);
                if (!x1 || !y1 || !x2 || !y2)
                    return fail("PXMOTION1027", "Cubic Bezier control coordinates are invalid");
                parsed.bezierX1 = *x1; parsed.bezierY1 = *y1;
                parsed.bezierX2 = *x2; parsed.bezierY2 = *y2;
            } else if (bezier != key.end())
                return fail("PXMOTION1027", "Bezier coordinates require a cubicBezier curve");
            if (*property == MotionProperty::Visible) {
                if (!key["value"].is_boolean() || *curve != MotionCurve::Step)
                    return fail("PXMOTION1018", "Visible motion keys require boolean step values");
                parsed.visible = key["value"].get<bool>();
            } else if (*property == MotionProperty::Frame) {
                const auto frameIndex = Number(key, "value", 0, 0, 4095, true);
                if (*curve != MotionCurve::Step || !frameIndex ||
                    std::floor(*frameIndex) != *frameIndex ||
                    *frameIndex >= static_cast<double>(clip.layers[track.layer].frames.size()))
                    return fail("PXMOTION1023", "Motion frame keys require an in-range integer step index");
                parsed.frame = static_cast<std::size_t>(*frameIndex);
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
    if (const auto markers = root.find("markers"); markers != root.end()) {
        if (clip.schemaRevision < 3 || !markers->is_array() || markers->size() > 4096)
            return fail("PXMOTION1024", "Motion audio markers require revision 3 and at most 4096 entries");
        std::unordered_set<std::string> markerIds;
        double previousTime = -1;
        clip.markers.reserve(markers->size());
        for (const auto& entry : *markers) {
            if (!OnlyFields(entry, {"id", "time", "audio", "bus"}) ||
                entry.size() != 4 || !entry.contains("id") ||
                !entry.contains("time") || !entry.contains("audio") ||
                !entry.contains("bus") || !entry["id"].is_string() ||
                !entry["audio"].is_string() || !entry["bus"].is_string())
                return fail("PXMOTION1025", "Motion audio marker shape is invalid");
            MotionAudioMarker marker;
            marker.id = entry["id"].get<std::string>();
            marker.audio = entry["audio"].get<std::string>();
            const auto time = Number(entry, "time", 0, 0, clip.duration, true);
            const auto bus = entry["bus"].get<std::string>();
            if (!Identifier(marker.id) || !markerIds.insert(marker.id).second ||
                !time || *time < previousTime || !SafePath(marker.audio) ||
                EndsWithIgnoreCase(marker.audio, ".pxmotion") ||
                EndsWithIgnoreCase(marker.audio, ".pxportrait") ||
                (bus != "se" && bus != "voice"))
                return fail("PXMOTION1026", "Motion audio marker id, time, path or bus is invalid");
            marker.time = *time;
            marker.bus = bus == "voice" ? MotionAudioBus::Voice : MotionAudioBus::SE;
            previousTime = *time;
            clip.markers.push_back(std::move(marker));
        }
    }
    result.clip = std::move(clip);
    return result;
}

std::vector<std::string> MotionDependencies(const MotionClip& clip) {
    std::vector<std::string> dependencies;
    std::unordered_set<std::string> seen;
    for (const auto& layer : clip.layers)
        if (layer.kind == MotionLayerKind::Image &&
            seen.insert(layer.image).second) dependencies.push_back(layer.image);
    return dependencies;
}

std::vector<std::string> MotionAudioDependencies(const MotionClip& clip) {
    std::vector<std::string> dependencies;
    std::unordered_set<std::string> seen;
    for (const auto& marker : clip.markers)
        if (seen.insert(marker.audio).second) dependencies.push_back(marker.audio);
    return dependencies;
}

MotionSampler::MotionSampler(const MotionClip& clip) : m_clip(clip) {
    m_states.resize(clip.layers.size());
    m_localTimes.resize(clip.layers.size());
    m_world.resize(clip.layers.size());
    m_pose.resize(clip.layers.size());
    Seek(0);
}

void MotionSampler::Seek(double timeSeconds) {
    const double finite = std::isfinite(timeSeconds) ? std::max(0.0, timeSeconds) : 0.0;
    m_position = m_clip.loop ? std::fmod(finite, m_clip.duration) :
        std::min(finite, m_clip.duration);
    for (const auto index : m_clip.hierarchyOrder) {
        const auto& layer = m_clip.layers[index];
        const double parentTime = layer.parent ? m_localTimes[*layer.parent] : m_position;
        m_localTimes[index] = parentTime * layer.timeScale + layer.timeOffset;
    }
    for (std::size_t i = 0; i < m_clip.layers.size(); ++i) {
        const auto& layer = m_clip.layers[i];
        m_states[i] = {layer.x, layer.y, layer.scaleX, layer.scaleY,
                       layer.rotation, layer.opacity, layer.visible, 0};
    }
    for (const auto& track : m_clip.tracks) {
        auto& state = m_states[track.layer];
        const double localTime = m_localTimes[track.layer];
        if (track.property == MotionProperty::Visible) {
            const auto right = std::upper_bound(track.keys.begin(), track.keys.end(), localTime,
                [](double at, const MotionKey& key) { return at < key.time; });
            if (right != track.keys.begin()) state.visible = (right - 1)->visible;
            continue;
        }
        if (track.property == MotionProperty::Frame) {
            const auto right = std::upper_bound(track.keys.begin(), track.keys.end(), localTime,
                [](double at, const MotionKey& key) { return at < key.time; });
            if (right != track.keys.begin()) state.frame = (right - 1)->frame;
            continue;
        }
        const auto sampled = SampleNumber(track, localTime);
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
            case MotionProperty::Frame: break;
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
        world.sourcePixels = state.frame < layer.frames.size()
            ? std::optional<MotionAtlasFrame>(layer.frames[state.frame]) : std::nullopt;
        world.opacity = state.opacity * (layer.parent ? m_world[*layer.parent].opacity : 1);
        world.visible = state.visible && (!layer.parent || m_world[*layer.parent].visible);
    }
    for (std::size_t i = 0; i < m_pose.size(); ++i) m_pose[i] = m_world[m_clip.drawOrder[i]];
}

}  // namespace px::sdk
