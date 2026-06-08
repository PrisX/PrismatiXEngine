#pragma once

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

#include "Engine/UI/UISchema.h"

namespace px::editor {

class UIDesigner {
public:
    void SetScene(px::ui::UIScene* scene, std::string path);
    void SetOnEdit(std::function<void()> cb) { m_onEdit = std::move(cb); }
    void SetOnStructureChange(std::function<void()> cb) { m_onStructure = std::move(cb); }

    void RenderHierarchy();
    void RenderInspector(const std::string& selectedAssetPath);
    void RenderAnimation();

    bool CanvasInput(ImVec2 p0, float scale, bool hovered, const std::string& selectedAssetPath);
    void DrawOverlay(ImVec2 p0, float scale);
    void AddImageAt(float canvasX, float canvasY, const std::string& image);

    bool Save();
    [[nodiscard]] bool Dirty() const { return m_dirty; }
    [[nodiscard]] const std::string& Path() const { return m_path; }

private:
    [[nodiscard]] px::Rect CanvasRect(const px::ui::UINode& node) const;
    void AnchorOffset(const px::ui::UINode& node, float& ox, float& oy) const;
    [[nodiscard]] int HitTest(float canvasX, float canvasY) const;
    void MarkEdited(bool structural = false);
    void AddNode(px::ui::NodeType type);
    void AddNodeAt(px::ui::NodeType type, float canvasX, float canvasY, const std::string& image = {});
    void AddBackgroundImage(const std::string& image);
    void RemoveSelectedNode();

    void SnapNode(px::ui::UINode& node, int handle, float scale);
    [[nodiscard]] int HandleHitTest(const px::Rect& canvasRect, float cx, float cy, float scale) const;

    px::ui::UIScene* m_scene = nullptr;
    std::string m_path;
    int m_selected = -1;
    bool m_dirty = false;

    bool m_dragging = false;
    int m_resizeHandle = 0;
    px::Rect m_rectStart;
    float m_grabCanvasX = 0.0f;
    float m_grabCanvasY = 0.0f;
    float m_contextCanvasX = 0.0f;
    float m_contextCanvasY = 0.0f;

    bool m_showGrid = false;
    int m_gridSize = 20;
    std::vector<float> m_guideX;
    std::vector<float> m_guideY;

    std::function<void()> m_onEdit;
    std::function<void()> m_onStructure;
};

}  // namespace px::editor
