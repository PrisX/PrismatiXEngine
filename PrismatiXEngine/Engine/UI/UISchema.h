#pragma once

#include "Engine/Core/Types.h"

#include <optional>
#include <string>
#include <vector>

namespace px::ui {

enum class NodeType {
    Frame,
    Panel,
    Button,
    Image,
    Text,
    DialogueBox,
    ChoiceList,
    Backlog,
    SaveGrid,
    GalleryGrid,
    Component,
};

enum class Anchor {
    TopLeft,
    Top,
    TopRight,
    Left,
    Center,
    Right,
    BottomLeft,
    Bottom,
    BottomRight,
};

struct AnimKey {
    float time = 0.0f;
    std::string property;
    float value = 0.0f;
};

struct AnimClip {
    std::string name;
    std::string trigger = "scene.enter";
    float duration = 0.4f;
    float delay = 0.0f;
    std::string easing = "outCubic";
    bool loop = false;
    std::vector<AnimKey> keys;
};

struct UINode {
    std::string id;
    NodeType type = NodeType::Panel;
    std::string layer;
    int order = 0;
    bool visible = true;
    bool locked = false;

    Rect rect;
    Anchor anchor = Anchor::TopLeft;
    float opacity = 255.0f;

    Color bgColor{ 20, 28, 42, 220 };
    Color hoverColor{ 40, 71, 87, 240 };
    Color borderColor{ 118, 144, 184, 120 };
    Color textColor{ 245, 248, 255, 255 };
    float radius = 0.0f;
    float borderTopHeight = 0.0f;
    bool textShadow = false;
    int outlineSize = 2;

    std::string text;
    std::string font = "Data/Font/NotoSansTC-Bold.ttf";
    int fontSize = 28;
    std::string align = "center";

    std::string image;
    std::string hoverImage;

    std::string actionType;
    std::string actionTarget;
    std::string actionArg;

    std::string component;
    std::string bind;

    std::vector<AnimClip> animations;
};

struct UIScene {
    int canvasW = 1280;
    int canvasH = 720;
    Color bgColor{ 0, 0, 0, 0 };
    std::string bgImage;
    std::uint8_t bgAlpha = 255;
    std::vector<UINode> nodes;
};

[[nodiscard]] std::optional<UIScene> ParsePXUI(const std::string& jsonText);
[[nodiscard]] std::string WritePXUI(const UIScene& scene);

[[nodiscard]] const char* ToString(NodeType type);
[[nodiscard]] NodeType NodeTypeFromString(const std::string& s);
[[nodiscard]] const char* ToString(Anchor anchor);
[[nodiscard]] Anchor AnchorFromString(const std::string& s);

}
