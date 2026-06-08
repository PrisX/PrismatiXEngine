#pragma once

#include "Engine/Common/Types.h"
#include "Engine/UI/UiSchema.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px {
class Input;
}
namespace px::gfx {
class Renderer2D;
}
namespace px::vn {
struct DialogueState;
}

namespace px::ui {

struct UiAction {
    std::string type;
    std::string target;
    std::string arg;
    std::string nodeId;
};

class UiStage {
public:
    void Load(UiScene scene);
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
        for (UiNode& n : m_scene.nodes) {
            if (n.id == id) {
                n.text = text;
                return;
            }
        }
    }

    std::optional<UiAction> Update(const Input& input, float dt);
    void Render(gfx::Renderer2D& renderer);

    [[nodiscard]] UiScene& Scene() { return m_scene; }
    [[nodiscard]] const UiScene& Scene() const { return m_scene; }

private:
    struct NodeAnim {
        float elapsed = 0.0f;
        bool active = false;
    };

    [[nodiscard]] Rect ScreenRect(const UiNode& node, gfx::Renderer2D& r) const;
    [[nodiscard]] Rect ScreenRect(const UiNode& node, int logicalW, int logicalH) const;
    [[nodiscard]] std::vector<Rect> GridCells(const UiNode& node, const Rect& area,
                                              std::size_t count) const;
    void ApplyAnim(const UiNode& node, const NodeAnim& anim, Rect& rect, float& opacity) const;
    void RenderNode(gfx::Renderer2D& r, const UiNode& node, std::size_t index);
    [[nodiscard]] std::vector<std::size_t> DrawOrder() const;

    UiScene m_scene;
    std::vector<NodeAnim> m_anims;
    const vn::DialogueState* m_dialogue = nullptr;
    const std::vector<std::string>* m_choices = nullptr;
    std::unordered_map<std::string, std::vector<GridItem>> m_grids;
    float m_mx = -1.0f;
    float m_my = -1.0f;
    bool m_editMode = false;
};

}
