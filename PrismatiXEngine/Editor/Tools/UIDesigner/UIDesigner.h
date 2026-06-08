#pragma once

#include "Engine/UI/UISchema.h"

#include <imgui.h>

#include <functional>
#include <string>

namespace px::editor {

class UIDesigner {
public:
    void SetScene(px::ui::UIScene* scene, std::string path);
    void SetOnEdit(std::function<void()> cb) { m_onEdit = std::move(cb); }
    void SetOnStructureChange(std::function<void()> cb) { m_onStructure = std::move(cb); }

    void RenderHierarchy();
    void RenderInspector(const std::string& selectedAssetPath);
    void RenderAnimation();

    bool CanvasInput(ImVec2 p0, float scale, bool hovered);
    void DrawOverlay(ImVec2 p0, float scale);

    bool Save();
    [[nodiscard]] bool Dirty() const { return m_dirty; }
    [[nodiscard]] const std::string& Path() const { return m_path; }

private:
    [[nodiscard]] px::Rect CanvasRect(const px::ui::UINode& node) const;
    [[nodiscard]] int HitTest(float canvasX, float canvasY) const;
    void MarkEdited(bool structural = false);
    void AddNode(px::ui::NodeType type);

    px::ui::UIScene* m_scene = nullptr;
    std::string m_path;
    int m_selected = -1;
    bool m_dirty = false;

    bool m_dragging = false;
    bool m_resizing = false;
    px::Rect m_rectStart;
    float m_grabCanvasX = 0.0f;
    float m_grabCanvasY = 0.0f;

    std::function<void()> m_onEdit;
    std::function<void()> m_onStructure;
};

}
