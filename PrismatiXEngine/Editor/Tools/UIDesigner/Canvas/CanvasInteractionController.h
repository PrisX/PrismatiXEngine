#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Types.h"
#include "Engine/UI/Control.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace px::editor {

class UIDesignerSession;

enum class DesignerMouseButton : std::uint8_t { None, Left, Right };

struct DesignerModifierKeys {
    bool shift = false;
    bool alt = false;
    bool controlOrCommand = false;
};

struct DesignerPointerEvent {
    Vec2 screenPosition{};
    Vec2 canvasPosition{};
    float zoom = 1.0f;
    DesignerMouseButton button = DesignerMouseButton::None;
    DesignerModifierKeys modifiers{};
    std::uint32_t clickCount = 1;
};

// The single authoritative non-ImGui Canvas interaction path. It owns pointer
// capture and every Canvas domain decision; UIDesigner only translates platform
// input and renders the state stored by UIDesignerSession.
class CanvasInteractionController {
public:
    using ImageSizeResolver = std::function<std::optional<Vec2>(const std::string&)>;

    explicit CanvasInteractionController(UIDesignerSession& session);

    void SetAnchorTool(bool anchors);
    [[nodiscard]] bool AnchorToolActive() const { return m_anchorTool; }
    void SetImageSizeResolver(ImageSizeResolver resolver) {
        m_imageSizeResolver = std::move(resolver);
    }

    void UpdateHover(const DesignerPointerEvent& event);
    bool PointerDown(const DesignerPointerEvent& event);
    bool PointerMove(const DesignerPointerEvent& event);
    bool PointerUp(const DesignerPointerEvent& event);
    void Cancel();
    [[nodiscard]] bool HasCapture() const { return m_hasCapture; }

private:
    [[nodiscard]] Uuid RootId() const;
    [[nodiscard]] Uuid Selected() const;
    [[nodiscard]] Vec2 CanvasSize() const;
    [[nodiscard]] Rect SelectedRect() const;
    [[nodiscard]] Rect ParentRect(const Uuid& node) const;
    [[nodiscard]] ui::ChildLayoutPolicy SelectedParentPolicy() const;
    [[nodiscard]] Uuid HitTest(Vec2 canvas) const;
    [[nodiscard]] Uuid NearestFreeAncestor(const Uuid& node) const;
    [[nodiscard]] std::size_t InsertionIndex(const Uuid& node, Vec2 canvas) const;
    [[nodiscard]] Rect SnapRect(Rect target, float zoom, bool disable,
                                std::span<const Uuid> ignored);

    void BeginFreeTransform(const Uuid& node, Vec2 canvas, int handle);
    void CommitManagedDrag(Vec2 canvas, bool detach);
    bool HandlePointerDown(const DesignerPointerEvent& event);
    bool HandlePointerMove(const DesignerPointerEvent& event);
    bool HandlePointerUp(const DesignerPointerEvent& event);
    static void Report(const Status& status);

    UIDesignerSession& m_session;
    ImageSizeResolver m_imageSizeResolver;
    bool m_anchorTool = false;
    bool m_hasCapture = false;
};

}  // namespace px::editor
