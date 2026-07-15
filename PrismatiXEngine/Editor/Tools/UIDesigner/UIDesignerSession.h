#pragma once

#include "Editor/Tools/UIDesigner/DesignerDocumentView.h"
#include "Editor/Tools/UIDesigner/DesignerSelection.h"
#include "Editor/Tools/UIDesigner/DocumentChangeSet.h"
#include "Editor/Tools/UIDesigner/Canvas/SnapEngine.h"
#include "Editor/Tools/UIDesigner/Preview/PreviewFixture.h"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace px::editor {

class CanvasInteractionController;
class DesignerCommandService;

enum class DesignerTool { Select, Anchors };

struct DesignerViewportState {
    DesignerTool tool = DesignerTool::Select;
    float zoom = 1.0f;
    float scrollX = 0.0f;
    float scrollY = 0.0f;
    bool fitToViewport = true;
    bool applyStoredScroll = true;
    bool gridVisible = false;
    bool gridSnap = false;
    bool smartGuides = true;
    bool showAllOutlines = false;
    bool interactivePreview = false;
    bool pixelExactPreview = false;
    int authorMode = 0;
    int animateSurface = 0;
    float gridSize = 16.0f;
    std::string devicePreset = "1280x720";
    std::string locale;
    float uiScale = 1.0f;
    float dpiScale = 1.0f;
};

struct DesignerTimelineState {
    Uuid selectedClip;
    float currentTime = 0.0f;
    bool playing = false;
    bool autoKey = false;
    int selectedTrack = -1;
    int selectedKey = -1;
    std::string property = "offsets";
};

struct DesignerBehaviorGraphState {
    Uuid selectedNode;
    Uuid selectedGroup;
    Uuid focusNode;
};

struct DesignerAnimationMachineState {
    Uuid selectedState;
    Uuid selectedTransition;
};

enum class DesignerCanvasGesture { None, Move, Resize, Anchors, Pivot, Reorder, Marquee };

struct DesignerCanvasState {
    DesignerCanvasGesture gesture = DesignerCanvasGesture::None;
    bool gestureDragged = false;
    int resizeHandle = 0;
    int anchorHandle = 0;
    int hoveredResizeHandle = 0;
    int hoveredAnchorHandle = 0;
    bool hoveredPivotHandle = false;
    Rect anchorsStart{};
    Rect anchorOffsetsStart{};
    Vec2 pivotStart{.5f, .5f};
    Variant authoredAnchorsStart;
    Variant authoredAnchorOffsetsStart;
    Variant authoredPivotStart;
    Vec2 dragStart{};
    Vec2 dragCurrent{};
    Vec2 marqueeCurrent{};
    bool marqueeAdditive = false;
    Rect rectStart{};
    std::unordered_map<Uuid, Rect, UuidHash> groupOffsetsStart;
    bool groupMove = false;
    std::size_t reorderPreview = 0;
    std::vector<SnapGuide> snapGuides;
    std::vector<SnapDistanceLabel> snapDistances;
    std::optional<DesignerLayoutSnapshot> layoutBefore;
    std::string hint;
    Vec2 contextPosition{};
    Uuid contextTarget;
};

struct DesignerPanelState {
    char treeFilter[96] = {0};
    char paletteFilter[96] = {0};
    char actionFilter[96] = {0};
    char treeRename[128] = {0};
    bool treeRenameOpen = false;
    bool createComponentOpen = false;
    bool detachConfirmOpen = false;
    char componentPath[260] = "Content/UI/Components/NewComponent.pxcomponent";
};

// One instance is the sole mutable editor state for one UI document.
class UIDesignerSession {
public:
    using ChangeListener = std::function<void(const DocumentChangeSet&)>;
    UIDesignerSession();
    ~UIDesignerSession();
    UIDesignerSession(const UIDesignerSession&) = delete;
    UIDesignerSession& operator=(const UIDesignerSession&) = delete;
    UIDesignerSession(UIDesignerSession&&) = delete;
    UIDesignerSession& operator=(UIDesignerSession&&) = delete;

    Status Open(const std::filesystem::path& path);
    Status New(const std::filesystem::path& path, int width = 1280, int height = 720);
    void Close();

    [[nodiscard]] bool HasDocument() const { return m_document != nullptr; }
    [[nodiscard]] UISceneDocument* Document() { return m_document.get(); }
    [[nodiscard]] const UISceneDocument* Document() const { return m_document.get(); }
    [[nodiscard]] DesignerDocumentView& DocumentView() { return m_documentView; }
    [[nodiscard]] const DesignerDocumentView& DocumentView() const { return m_documentView; }
    [[nodiscard]] DesignerSelection& Selection() { return m_selection; }
    [[nodiscard]] const DesignerSelection& Selection() const { return m_selection; }
    [[nodiscard]] CanvasInteractionController& Interaction();
    [[nodiscard]] const CanvasInteractionController& Interaction() const;
    [[nodiscard]] DesignerCommandService& Commands();
    [[nodiscard]] const DesignerCommandService& Commands() const;
    void SetChangeListener(ChangeListener listener);
    [[nodiscard]] bool HistoryDirty() const;
    [[nodiscard]] std::size_t HistoryCursor() const;

    void MarkDirty(DesignerDirtyFlags flags) { m_dirtyFlags |= flags; }
    [[nodiscard]] DesignerDirtyFlags DirtyFlags() const { return m_dirtyFlags; }
    [[nodiscard]] DesignerDirtyFlags ConsumeDirtyFlags();
    void ClearDirtyFlags() { m_dirtyFlags = DesignerDirtyFlags::None; }
    [[nodiscard]] DocumentChangeSet ConsumeDocumentChanges();

    DesignerViewportState viewport;
    DesignerTimelineState timeline;
    DesignerBehaviorGraphState behaviorGraph;
    DesignerAnimationMachineState animationMachine;
    DesignerCanvasState canvas;
    DesignerPanelState panels;
    PreviewFixture previewFixture;
    VariantObject clipboardSubtree;
    std::string selectedSignal;
    Uuid hoveredNode;
    std::unordered_set<Uuid, UuidHash> expandedTreeNodes;
    std::string inspectorSearch;
    std::chrono::steady_clock::time_point lastEdit = std::chrono::steady_clock::now();

private:
    Status Adopt(std::unique_ptr<UISceneDocument> document);
    void ResetViewState();

    std::unique_ptr<UISceneDocument> m_document;
    DesignerDocumentView m_documentView;
    DesignerSelection m_selection;
    std::unique_ptr<CanvasInteractionController> m_interaction;
    std::unique_ptr<DesignerCommandService> m_commands;
    DesignerDirtyFlags m_dirtyFlags = DesignerDirtyFlags::None;
    DocumentChangeSet m_pendingChanges;
    ChangeListener m_changeListener;
};

}  // namespace px::editor
