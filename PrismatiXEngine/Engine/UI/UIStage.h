#pragma once

#include "Engine/Core/Types.h"
#include "Engine/UI/UISchema.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px {
class Input;
}
namespace px::graphics {
class Renderer2D;
}
namespace px::vn {
struct DialogueState;
}

namespace px::ui {

struct UIAction {
    std::string type;
    std::string target;
    std::string arg;
    std::string nodeId;
};

class UIStage {
public:
    void Load(UIScene scene);
    void TriggerEnter();

    void SetEditMode(bool edit) { m_editMode = edit; }
    void OnSceneEdited() { m_anims.resize(m_scene.nodes.size()); }

    void SetDialogue(const vn::DialogueState* dialogue) { m_dialogue = dialogue; }
    void SetChoices(const std::vector<std::string>* choices) { m_choices = choices; }

    struct GridItem {
        std::string label;
        std::string image;
        bool locked = false;
        std::string actionType;
        std::string actionArg;
    };
    void SetGrid(const std::string& bind, std::vector<GridItem> items) {
        m_grids[bind] = std::move(items);
    }
    void SetNodeText(const std::string& id, const std::string& text) {
        for (UINode& n : m_scene.nodes) {
            if (n.id == id) {
                n.text = text;
                return;
            }
        }
    }

    std::optional<UIAction> Update(const Input& input, float dt);
    void Render(graphics::Renderer2D& renderer);

    [[nodiscard]] UIScene& Scene() { return m_scene; }
    [[nodiscard]] const UIScene& Scene() const { return m_scene; }

private:
    struct NodeAnim {
        float elapsed = 0.0f;
        bool active = false;
    };

    [[nodiscard]] Rect ScreenRect(const UINode& node, graphics::Renderer2D& r) const;
    [[nodiscard]] Rect ScreenRect(const UINode& node, int logicalW, int logicalH) const;
    [[nodiscard]] std::vector<Rect> GridCells(const UINode& node, const Rect& area,
                                              std::size_t count) const;
    void ApplyAnim(const UINode& node, const NodeAnim& anim, Rect& rect, float& opacity) const;
    void RenderNode(graphics::Renderer2D& r, const UINode& node, std::size_t index);
    [[nodiscard]] std::vector<std::size_t> DrawOrder() const;

    UIScene m_scene;
    std::vector<NodeAnim> m_anims;
    const vn::DialogueState* m_dialogue = nullptr;
    const std::vector<std::string>* m_choices = nullptr;
    std::unordered_map<std::string, std::vector<GridItem>> m_grids;
    float m_mx = -1.0f;
    float m_my = -1.0f;
    int m_logicalW = 1280;
    int m_logicalH = 720;
    bool m_editMode = false;
};

}
