#pragma once

#include "Editor/Tools/UIDesigner/DesignerDocumentView.h"
#include "Editor/Tools/UIDesigner/DesignerSelection.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace px::editor {

class CanvasInteractionController;
class DesignerCommandService;

enum class DesignerDirtyFlags : std::uint32_t {
    None = 0,
    Structure = 1u << 0,
    Layout = 1u << 1,
    Paint = 1u << 2,
    Binding = 1u << 3,
    Theme = 1u << 4,
    PreviewState = 1u << 5,
    Animation = 1u << 6,
};

constexpr DesignerDirtyFlags operator|(DesignerDirtyFlags lhs, DesignerDirtyFlags rhs) {
    return static_cast<DesignerDirtyFlags>(static_cast<std::uint32_t>(lhs) |
                                           static_cast<std::uint32_t>(rhs));
}
constexpr DesignerDirtyFlags operator&(DesignerDirtyFlags lhs, DesignerDirtyFlags rhs) {
    return static_cast<DesignerDirtyFlags>(static_cast<std::uint32_t>(lhs) &
                                           static_cast<std::uint32_t>(rhs));
}
constexpr DesignerDirtyFlags& operator|=(DesignerDirtyFlags& lhs, DesignerDirtyFlags rhs) {
    lhs = lhs | rhs;
    return lhs;
}
[[nodiscard]] constexpr bool Any(DesignerDirtyFlags flags) {
    return flags != DesignerDirtyFlags::None;
}
[[nodiscard]] constexpr bool HasDirtyFlag(DesignerDirtyFlags flags, DesignerDirtyFlags flag) {
    return Any(flags & flag);
}

inline constexpr DesignerDirtyFlags kAllDesignerContentDirty =
    DesignerDirtyFlags::Structure | DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint |
    DesignerDirtyFlags::Binding | DesignerDirtyFlags::Theme | DesignerDirtyFlags::Animation;

struct DesignerSessionViewportState {
    float zoom = 1.0f;
    float scrollX = 0.0f;
    float scrollY = 0.0f;
    bool fitToViewport = true;
};

struct DesignerGuideState {
    enum class Orientation { Vertical, Horizontal };
    Orientation orientation = Orientation::Vertical;
    float position = 0.0f;
    bool locked = false;
};

struct DesignerGridGuideSettings {
    bool gridVisible = false;
    bool gridSnap = false;
    float gridSize = 16.0f;
    bool smartGuides = true;
    bool guidesVisible = true;
    bool guidesLocked = false;
    std::vector<DesignerGuideState> guides;
};

struct DesignerPreviewState {
    bool interactive = false;
    std::string devicePreset = "1280x720";
    std::string locale;
    float uiScale = 1.0f;
    float dpiScale = 1.0f;
};

struct DesignerTimelineState {
    std::string activeClip;
    float currentTime = 0.0f;
    bool playing = false;
    bool autoKey = false;
};

// One instance is the sole mutable editor state for one UI document.
class UIDesignerSession {
public:
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

    void MarkDirty(DesignerDirtyFlags flags) { m_dirtyFlags |= flags; }
    [[nodiscard]] DesignerDirtyFlags DirtyFlags() const { return m_dirtyFlags; }
    [[nodiscard]] DesignerDirtyFlags ConsumeDirtyFlags();
    void ClearDirtyFlags() { m_dirtyFlags = DesignerDirtyFlags::None; }

    DesignerSessionViewportState viewport;
    DesignerGridGuideSettings gridAndGuides;
    DesignerPreviewState preview;
    DesignerTimelineState timeline;
    Uuid hoveredNode;
    std::unordered_set<Uuid, UuidHash> expandedTreeNodes;
    std::string inspectorSearch;
    float bottomDrawerHeight = 260.0f;
    bool bottomDrawerExpanded = false;

private:
    Status Adopt(std::unique_ptr<UISceneDocument> document);
    void ResetViewState();

    std::unique_ptr<UISceneDocument> m_document;
    DesignerDocumentView m_documentView;
    DesignerSelection m_selection;
    std::unique_ptr<CanvasInteractionController> m_interaction;
    std::unique_ptr<DesignerCommandService> m_commands;
    DesignerDirtyFlags m_dirtyFlags = DesignerDirtyFlags::None;
};

}  // namespace px::editor
