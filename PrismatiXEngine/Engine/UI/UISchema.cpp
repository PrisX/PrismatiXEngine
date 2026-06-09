#include "Engine/UI/UISchema.h"

#include <nlohmann/json.hpp>

#include <array>
#include <unordered_map>

namespace px::ui {

using Json = nlohmann::json;

namespace {

const std::array<const char*, 11> kNodeNames = {
    "frame",   "panel",       "button",      "image",     "text",     "dialogue_box",
    "choice_list", "backlog",  "save_grid",   "gallery_grid", "component"
};

const std::array<const char*, 9> kAnchorNames = {
    "topleft", "top", "topright", "left", "center", "right", "bottomleft", "bottom", "bottomright"
};

Json ColorToJson(Color c) {
    return Json::array({ c.r, c.g, c.b, c.a });
}

Color ColorFromJson(const Json& j, Color fallback) {
    if (!j.is_array() || j.size() < 3) {
        return fallback;
    }
    Color c;
    c.r = j[0].get<int>();
    c.g = j[1].get<int>();
    c.b = j[2].get<int>();
    c.a = j.size() >= 4 ? j[3].get<int>() : 255;
    return c;
}

}

const char* ToString(NodeType type) {
    const auto i = static_cast<std::size_t>(type);
    return i < kNodeNames.size() ? kNodeNames[i] : "panel";
}

NodeType NodeTypeFromString(const std::string& s) {
    for (std::size_t i = 0; i < kNodeNames.size(); ++i) {
        if (s == kNodeNames[i]) return static_cast<NodeType>(i);
    }
    return NodeType::Panel;
}

const char* ToString(Anchor anchor) {
    const auto i = static_cast<std::size_t>(anchor);
    return i < kAnchorNames.size() ? kAnchorNames[i] : "topleft";
}

Anchor AnchorFromString(const std::string& s) {
    for (std::size_t i = 0; i < kAnchorNames.size(); ++i) {
        if (s == kAnchorNames[i]) return static_cast<Anchor>(i);
    }
    return Anchor::TopLeft;
}

namespace {

Json NodeToJson(const UINode& n) {
    Json j;
    j["id"] = n.id;
    j["type"] = ToString(n.type);
    j["layer"] = n.layer;
    j["order"] = n.order;
    j["visible"] = n.visible;
    j["locked"] = n.locked;
    j["rect"] = { n.rect.x, n.rect.y, n.rect.w, n.rect.h };
    j["anchor"] = ToString(n.anchor);
    j["opacity"] = n.opacity;
    j["bgColor"] = ColorToJson(n.bgColor);
    j["hoverColor"] = ColorToJson(n.hoverColor);
    j["borderColor"] = ColorToJson(n.borderColor);
    j["textColor"] = ColorToJson(n.textColor);
    j["radius"] = n.radius;
    j["borderTopHeight"] = n.borderTopHeight;
    j["textShadow"] = n.textShadow;
    j["outlineSize"] = n.outlineSize;
    j["text"] = n.text;
    j["font"] = n.font;
    j["fontSize"] = n.fontSize;
    j["align"] = n.align;
    j["image"] = n.image;
    j["hoverImage"] = n.hoverImage;
    j["actionType"] = n.actionType;
    j["actionTarget"] = n.actionTarget;
    j["actionArg"] = n.actionArg;
    j["component"] = n.component;
    j["bind"] = n.bind;
    Json anims = Json::array();
    for (const AnimClip& c : n.animations) {
        Json jc;
        jc["name"] = c.name;
        jc["trigger"] = c.trigger;
        jc["duration"] = c.duration;
        jc["delay"] = c.delay;
        jc["easing"] = c.easing;
        jc["loop"] = c.loop;
        Json keys = Json::array();
        for (const AnimKey& k : c.keys) {
            keys.push_back({ { "time", k.time }, { "property", k.property }, { "value", k.value } });
        }
        jc["keys"] = keys;
        anims.push_back(jc);
    }
    j["animations"] = anims;
    return j;
}

UINode NodeFromJson(const Json& j) {
    UINode n;
    n.id = j.value("id", std::string{});
    n.type = NodeTypeFromString(j.value("type", std::string{ "panel" }));
    n.layer = j.value("layer", std::string{});
    n.order = j.value("order", 0);
    n.visible = j.value("visible", true);
    n.locked = j.value("locked", false);
    if (j.contains("rect") && j["rect"].is_array() && j["rect"].size() >= 4) {
        n.rect = Rect{ j["rect"][0].get<float>(), j["rect"][1].get<float>(),
                       j["rect"][2].get<float>(), j["rect"][3].get<float>() };
    }
    n.anchor = AnchorFromString(j.value("anchor", std::string{ "topleft" }));
    n.opacity = j.value("opacity", 255.0f);
    n.bgColor = ColorFromJson(j.value("bgColor", Json{}), n.bgColor);
    n.hoverColor = ColorFromJson(j.value("hoverColor", Json{}), n.hoverColor);
    n.borderColor = ColorFromJson(j.value("borderColor", Json{}), n.borderColor);
    n.textColor = ColorFromJson(j.value("textColor", Json{}), n.textColor);
    n.radius = j.value("radius", 0.0f);
    n.borderTopHeight = j.value("borderTopHeight", 0.0f);
    n.textShadow = j.value("textShadow", false);
    n.outlineSize = j.value("outlineSize", 2);
    n.text = j.value("text", std::string{});
    n.font = j.value("font", n.font);
    n.fontSize = j.value("fontSize", 28);
    n.align = j.value("align", std::string{ "center" });
    n.image = j.value("image", std::string{});
    n.hoverImage = j.value("hoverImage", std::string{});
    n.actionType = j.value("actionType", std::string{});
    n.actionTarget = j.value("actionTarget", std::string{});
    n.actionArg = j.value("actionArg", std::string{});
    n.component = j.value("component", std::string{});
    n.bind = j.value("bind", std::string{});
    if (j.contains("animations")) {
        for (const Json& jc : j["animations"]) {
            AnimClip c;
            c.name = jc.value("name", std::string{});
            c.trigger = jc.value("trigger", std::string{ "scene.enter" });
            c.duration = jc.value("duration", 0.4f);
            c.delay = jc.value("delay", 0.0f);
            c.easing = jc.value("easing", std::string{ "outCubic" });
            c.loop = jc.value("loop", false);
            if (jc.contains("keys")) {
                for (const Json& jk : jc["keys"]) {
                    c.keys.push_back(AnimKey{ jk.value("time", 0.0f),
                                              jk.value("property", std::string{}),
                                              jk.value("value", 0.0f) });
                }
            }
            n.animations.push_back(std::move(c));
        }
    }
    return n;
}

}

std::optional<UIScene> ParsePXUI(const std::string& jsonText) {
    Json j = Json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        return std::nullopt;
    }
    UIScene scene;
    if (j.contains("canvas")) {
        scene.canvasW = j["canvas"].value("w", 1280);
        scene.canvasH = j["canvas"].value("h", 720);
    }
    if (j.contains("background")) {
        const Json& bg = j["background"];
        scene.bgColor = ColorFromJson(bg.value("color", Json{}), scene.bgColor);
        scene.bgImage = bg.value("image", std::string{});
        scene.bgAlpha = static_cast<std::uint8_t>(bg.value("alpha", 255));
    }
    if (j.contains("nodes")) {
        for (const Json& jn : j["nodes"]) {
            scene.nodes.push_back(NodeFromJson(jn));
        }
    }
    return scene;
}

std::string WritePXUI(const UIScene& scene) {
    Json j;
    j["canvas"] = { { "w", scene.canvasW }, { "h", scene.canvasH } };
    j["background"] = { { "color", ColorToJson(scene.bgColor) },
                        { "image", scene.bgImage },
                        { "alpha", scene.bgAlpha } };
    Json nodes = Json::array();
    for (const UINode& n : scene.nodes) {
        nodes.push_back(NodeToJson(n));
    }
    j["nodes"] = nodes;
    return j.dump(2);
}

}
