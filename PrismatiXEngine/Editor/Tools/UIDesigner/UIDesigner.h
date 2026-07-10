#pragma once

#include "Editor/Tools/UIDesigner/UISceneDocument.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Binding.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <functional>
#include <chrono>
#include <memory>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace px::editor {

enum class DesignerTool { Select, Move, Resize, Anchors };

struct DesignerViewportState {
    DesignerTool tool = DesignerTool::Select;
    ImVec2 pan{};
    float zoom = 1.0f;
    bool gridVisible = false;
    bool gridSnap = false;
    bool smartGuides = true;
    bool showAllOutlines = false;
    bool interactivePreview = false;
};

class UIDesigner {
public:
    UIDesigner();
    UIDesigner(const UIDesigner&) = delete;
    UIDesigner& operator=(const UIDesigner&) = delete;
    UIDesigner(UIDesigner&&) noexcept = default;
    UIDesigner& operator=(UIDesigner&&) noexcept = default;

    Status Open(const std::filesystem::path& path);
    Status New(const std::filesystem::path& path, int width = 1280, int height = 720);
    void SetOnEdit(std::function<void()> cb) { m_onEdit = std::move(cb); }
    void SetOnStructureChange(std::function<void()> cb) { m_onStructure = std::move(cb); }

    void RenderHierarchy();
    void RenderInspector(const std::string& selectedAssetPath);
    void RenderAnimation();
    void RenderTheme();
    void RenderViewportToolbar();
    bool CanvasInput(const ImRect& viewport, ImVec2 p0, float scale, bool hovered,
                     const std::string& selectedAssetPath);
    void DrawOverlay(ImVec2 p0, float scale);
    void AddImageAt(float canvasX, float canvasY, const std::string& image);

    bool Save();
    [[nodiscard]] bool Dirty() const { return m_document && m_document->History().Dirty(); }
    [[nodiscard]] const std::string& Path() const { return m_pathText; }
    [[nodiscard]] UISceneDocument* Document() const { return m_document.get(); }
    [[nodiscard]] bool HasDocument() const { return m_document != nullptr; }
    [[nodiscard]] std::chrono::steady_clock::time_point LastEditTime() const { return m_lastEdit; }
    [[nodiscard]] const DesignerViewportState& ViewportState() const { return m_viewport; }
    [[nodiscard]] DesignerViewportState& ViewportState() { return m_viewport; }
    [[nodiscard]] Vec2 CanvasSize() const;
    [[nodiscard]] std::string SelectionSummary() const;
    [[nodiscard]] ui::ChildLayoutPolicy SelectedParentPolicy() const;
    Status Undo();
    Status Redo();
    void RelocateDocument(const std::filesystem::path& oldPath,
                          const std::filesystem::path& newPath);

private:
    void RenderTreeNode(resource::NodeRecord& record);
    [[nodiscard]] bool TreeMatches(const resource::NodeRecord& record) const;
    void RebuildLayout();
    void AddNode(std::string type, Vec2 canvasPosition = {}, std::string image = {});
    void RemoveSelected();
    void DuplicateSelected();
    void MarkEdited(bool structural = false);
    void EditVariant(const char* label, const std::string& property, Variant before, Variant value,
                     bool changed, bool continuous);
    [[nodiscard]] Uuid RootId() const;
    [[nodiscard]] Uuid ParentForNewNode() const;
    [[nodiscard]] Rect SelectedRect() const;
    [[nodiscard]] Rect ParentRect(const Uuid& node) const;
    [[nodiscard]] Uuid HitTest(Vec2 canvas) const;
    [[nodiscard]] Uuid NearestFreeAncestor(const Uuid& node) const;
    [[nodiscard]] std::size_t InsertionIndex(const Uuid& node, Vec2 canvas) const;
    void BeginFreeTransform(const Uuid& node, Vec2 canvas, int handle);
    void CommitManagedDrag(Vec2 canvas, bool detach);
    void CancelCanvasGesture();
    [[nodiscard]] static VariantObject NewSubtree(std::string type, std::string name, Rect anchors,
                                                  Rect offsets, std::string image = {});
    static void RegenerateIds(VariantObject& subtree);

    std::unique_ptr<UISceneDocument> m_document;
    std::string m_pathText;
    Uuid m_selected;
    std::unordered_set<Uuid, UuidHash> m_selection;
    Uuid m_hovered;
    std::unordered_map<Uuid, Rect, UuidHash> m_layout;
    std::unordered_map<Uuid, ui::ChildLayoutPolicy, UuidHash> m_childPolicies;
    std::unique_ptr<PropertyEditTransaction> m_propertyTransaction;
    std::string m_transactionProperty;
    Uuid m_transactionTarget;
    enum class Gesture { None, Move, Resize, Anchors, Reorder, Pan };
    Gesture m_gesture = Gesture::None;
    int m_resizeHandle = 0;
    int m_anchorHandle = 0;
    Rect m_anchorsStart{};
    Rect m_anchorOffsetsStart{};
    Vec2 m_dragStart{};
    Rect m_rectStart{};
    Rect m_offsetsStart{};
    std::unordered_map<Uuid, Rect, UuidHash> m_groupOffsetsStart;
    bool m_groupMove = false;
    int m_gridSize = 16;
    std::size_t m_reorderPreview = 0;
    float m_guideX = std::numeric_limits<float>::quiet_NaN();
    float m_guideY = std::numeric_limits<float>::quiet_NaN();
    std::string m_canvasHint;
    char m_treeFilter[96] = {0};
    char m_treeRename[128] = {0};
    bool m_treeRenameOpen = false;
    DesignerViewportState m_viewport;
    std::function<void()> m_onEdit;
    std::function<void()> m_onStructure;
    ui::FormatterRegistry m_formatters;
    ui::AnimationClip m_animation;
    std::chrono::steady_clock::time_point m_lastEdit = std::chrono::steady_clock::now();
};

struct DesignerDocumentSession {
    std::filesystem::path canonicalPath;
    std::unique_ptr<UIDesigner> editor;
};

}  // namespace px::editor
