#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"

#include "Editor/Tools/UIDesigner/Canvas/HitTestService.h"
#include "Editor/Tools/UIDesigner/Canvas/SnapEngine.h"
#include "Editor/Tools/UIDesigner/DesignerCommandService.h"
#include "Editor/Tools/UIDesigner/UIDesignerSession.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace px::editor {

CanvasInteractionController::CanvasInteractionController(UIDesignerSession& session)
    : m_session(session) {}

void CanvasInteractionController::Report(const Status& status) {
    for (auto diagnostic : status.Diagnostics()) diag::Emit(std::move(diagnostic));
}

Uuid CanvasInteractionController::RootId() const {
    return m_session.DocumentView().Root();
}

Uuid CanvasInteractionController::Selected() const {
    return m_session.Selection().Primary();
}

Vec2 CanvasInteractionController::CanvasSize() const {
    const auto* document = m_session.Document();
    if (document) {
        const auto found = document->Data().properties.find("canvasSize");
        if (found != document->Data().properties.end())
            if (const auto* size = found->second.TryGet<Vec2>()) return *size;
    }
    return {1280, 720};
}

Rect CanvasInteractionController::SelectedRect() const {
    const auto rect = m_session.DocumentView().LayoutRect(Selected());
    return rect ? *rect : Rect{};
}

Rect CanvasInteractionController::ParentRect(const Uuid& nodeId) const {
    const auto* document = m_session.Document();
    if (!document) return {};
    const auto* node = m_session.DocumentView().Find(*document, nodeId);
    if (!node || node->parent.Empty())
        return {0, 0, CanvasSize().x, CanvasSize().y};
    const auto rect = m_session.DocumentView().LayoutRect(node->parent);
    return rect ? *rect : Rect{0, 0, CanvasSize().x, CanvasSize().y};
}

ui::ChildLayoutPolicy CanvasInteractionController::SelectedParentPolicy() const {
    const auto* document = m_session.Document();
    if (!document) return ui::ChildLayoutPolicy::Free;
    const auto* node = m_session.DocumentView().Find(*document, Selected());
    if (!node || node->parent.Empty()) return ui::ChildLayoutPolicy::Free;
    const auto policy = m_session.DocumentView().ChildPolicy(node->parent);
    return policy ? *policy : ui::ChildLayoutPolicy::Free;
}

Uuid CanvasInteractionController::HitTest(Vec2 canvas) const {
    const auto* document = m_session.Document();
    return document ? HitTestService{}.Topmost(
                          *document, m_session.DocumentView(), canvas,
                          m_session.Selection().Scope())
                    : Uuid{};
}

Uuid CanvasInteractionController::NearestFreeAncestor(const Uuid& nodeId) const {
    const auto* document = m_session.Document();
    if (!document) return {};
    const auto* node = m_session.DocumentView().Find(*document, nodeId);
    Uuid candidate = node ? node->parent : Uuid{};
    while (!candidate.Empty()) {
        const auto policy = m_session.DocumentView().ChildPolicy(candidate);
        if (!policy || *policy == ui::ChildLayoutPolicy::Free) return candidate;
        candidate = m_session.DocumentView().Parent(candidate);
    }
    return {};
}

std::size_t CanvasInteractionController::InsertionIndex(const Uuid& nodeId,
                                                        Vec2 canvas) const {
    const auto* document = m_session.Document();
    if (!document) return 0;
    const auto* node = m_session.DocumentView().Find(*document, nodeId);
    if (!node) return 0;
    const auto policy = SelectedParentPolicy();
    std::vector<Uuid> siblings;
    for (const Uuid& sibling : m_session.DocumentView().Children(node->parent))
        if (sibling != nodeId) siblings.push_back(sibling);
    for (std::size_t index = 0; index < siblings.size(); ++index) {
        const auto rect = m_session.DocumentView().LayoutRect(siblings[index]);
        if (!rect) continue;
        if (policy == ui::ChildLayoutPolicy::LinearX) {
            if (canvas.x < rect->x + rect->w * 0.5f) return index;
        } else if (policy == ui::ChildLayoutPolicy::Grid ||
                   policy == ui::ChildLayoutPolicy::Flow) {
            if (canvas.y < rect->y + rect->h * 0.5f ||
                (std::abs(canvas.y - (rect->y + rect->h * 0.5f)) <
                     rect->h * 0.4f &&
                 canvas.x < rect->x + rect->w * 0.5f))
                return index;
        } else if (canvas.y < rect->y + rect->h * 0.5f) {
            return index;
        }
    }
    return siblings.size();
}

Rect CanvasInteractionController::SnapRect(Rect target, float zoom, bool disable,
                                           std::span<const Uuid> ignored) {
    auto* document = m_session.Document();
    auto& state = m_session.canvas;
    const auto* selected =
        document ? m_session.DocumentView().Find(*document, Selected()) : nullptr;
    if (!selected || disable ||
        (!m_session.viewport.smartGuides && !m_session.viewport.gridSnap))
        return target;
    const bool resize = state.gesture == DesignerCanvasGesture::Resize;
    const bool moveLeft =
        state.resizeHandle == 1 || state.resizeHandle == 7 || state.resizeHandle == 8;
    const bool moveRight =
        state.resizeHandle == 3 || state.resizeHandle == 4 || state.resizeHandle == 5;
    const bool moveTop = state.resizeHandle >= 1 && state.resizeHandle <= 3;
    const bool moveBottom = state.resizeHandle >= 5 && state.resizeHandle <= 7;
    SnapRequest request{.movingRect = target,
                        .mode = resize ? SnapMode::Resize : SnapMode::Move,
                        .parent = selected->parent,
                        .ignoredNodes = ignored,
                        .zoom = std::max(0.01f, zoom),
                        .canvasRect = {0, 0, CanvasSize().x, CanvasSize().y},
                        .alignmentEnabled = m_session.viewport.smartGuides,
                        .gridEnabled = m_session.viewport.gridSnap,
                        .gridSize = m_session.viewport.gridSize,
                        .snapLeft = !resize || moveLeft,
                        .snapRight = !resize || moveRight,
                        .snapTop = !resize || moveTop,
                        .snapBottom = !resize || moveBottom};
    SnapResult result = SnapEngine{}.Snap(request, m_session.DocumentView());
    state.snapGuides = std::move(result.guides);
    state.snapDistances = std::move(result.distances);
    return result.rect;
}

void CanvasInteractionController::SetAnchorTool(bool anchors) {
    if (m_anchorTool == anchors) return;
    Cancel();
    m_anchorTool = anchors;
}

void CanvasInteractionController::UpdateHover(const DesignerPointerEvent& event) {
    auto& state = m_session.canvas;
    auto* document = m_session.Document();
    if (!document) {
        m_session.hoveredNode = {};
        state.hoveredResizeHandle = 0;
        state.hoveredAnchorHandle = 0;
        state.hoveredPivotHandle = false;
        return;
    }
    m_session.hoveredNode = HitTest(event.canvasPosition);
    int resizeHandle = 0;
    if (!Selected().Empty() &&
        SelectedParentPolicy() == ui::ChildLayoutPolicy::Free) {
        const Rect rect = SelectedRect();
        const float hitSize = 7.0f / std::max(0.01f, event.zoom);
        const std::array<Vec2, 8> points{{
            {rect.x, rect.y},
            {rect.x + rect.w * 0.5f, rect.y},
            {rect.x + rect.w, rect.y},
            {rect.x + rect.w, rect.y + rect.h * 0.5f},
            {rect.x + rect.w, rect.y + rect.h},
            {rect.x + rect.w * 0.5f, rect.y + rect.h},
            {rect.x, rect.y + rect.h},
            {rect.x, rect.y + rect.h * 0.5f},
        }};
        for (int index = 0; index < 8; ++index) {
            if (std::abs(event.canvasPosition.x - points[index].x) <= hitSize &&
                std::abs(event.canvasPosition.y - points[index].y) <= hitSize) {
                resizeHandle = index + 1;
                break;
            }
        }
    }

    int anchorHandle = 0;
    if (m_anchorTool && !Selected().Empty() && Selected() != RootId() &&
        SelectedParentPolicy() == ui::ChildLayoutPolicy::Free) {
        const auto* node = m_session.DocumentView().Find(*document, Selected());
        Rect anchors{};
        if (node) {
            const auto found = node->properties.find("anchors");
            if (found != node->properties.end())
                if (const auto* value = found->second.TryGet<Rect>()) anchors = *value;
        }
        const Rect parent = ParentRect(Selected());
        const float hitSize = 8.0f / std::max(0.01f, event.zoom);
        const std::array<Vec2, 4> points{{
            {parent.x + parent.w * anchors.x, parent.y + parent.h * anchors.y},
            {parent.x + parent.w * anchors.w, parent.y + parent.h * anchors.y},
            {parent.x + parent.w * anchors.w, parent.y + parent.h * anchors.h},
            {parent.x + parent.w * anchors.x, parent.y + parent.h * anchors.h},
        }};
        for (int index = 0; index < 4; ++index) {
            if (std::abs(event.canvasPosition.x - points[index].x) <= hitSize &&
                std::abs(event.canvasPosition.y - points[index].y) <= hitSize) {
                anchorHandle = index + 1;
                break;
            }
        }
    }

    bool pivotHandle = false;
    if (!m_anchorTool && !Selected().Empty() && Selected() != RootId() &&
        m_session.Selection().Size() == 1 &&
        SelectedParentPolicy() == ui::ChildLayoutPolicy::Free) {
        const auto* node = m_session.DocumentView().Find(*document, Selected());
        Vec2 pivot{0.5f, 0.5f};
        if (node) {
            const auto found = node->properties.find("pivot");
            if (found != node->properties.end())
                if (const auto* value = found->second.TryGet<Vec2>()) pivot = *value;
        }
        const Rect selected = SelectedRect();
        const Vec2 point{selected.x + selected.w * pivot.x,
                         selected.y + selected.h * pivot.y};
        const float hitSize = 9.0f / std::max(0.01f, event.zoom);
        pivotHandle =
            std::abs(event.canvasPosition.x - point.x) <= hitSize &&
            std::abs(event.canvasPosition.y - point.y) <= hitSize;
        if (pivotHandle) resizeHandle = 0;
    }

    state.hoveredResizeHandle = resizeHandle;
    state.hoveredAnchorHandle = anchorHandle;
    state.hoveredPivotHandle = pivotHandle;
}

void CanvasInteractionController::BeginFreeTransform(const Uuid& nodeId, Vec2 canvas,
                                                     int handle) {
    auto* document = m_session.Document();
    if (!document) return;
    auto& selection = m_session.Selection();
    if (!selection.SetPrimary(nodeId)) selection.Replace(nodeId);
    selection.Canonicalize(m_session.DocumentView());
    const Uuid target = Selected();
    auto& state = m_session.canvas;
    state.dragStart = canvas;
    state.rectStart = SelectedRect();
    state.resizeHandle = handle;
    state.gesture =
        handle == 0 ? DesignerCanvasGesture::Move : DesignerCanvasGesture::Resize;
    state.gestureDragged = false;
    state.groupMove = false;
    state.groupOffsetsStart.clear();
    state.layoutBefore.reset();

    if (handle == 0 && selection.Size() > 1) {
        const auto* primary = m_session.DocumentView().Find(*document, target);
        for (const Uuid& id : selection.OrderedItems()) {
            const auto* candidate = m_session.DocumentView().Find(*document, id);
            if (!primary || !candidate || candidate->parent != primary->parent) continue;
            const auto policy = m_session.DocumentView().ChildPolicy(candidate->parent);
            if (policy && *policy != ui::ChildLayoutPolicy::Free) continue;
            const auto property = candidate->properties.find("offsets");
            if (property != candidate->properties.end())
                if (const auto* value = property->second.TryGet<Rect>())
                    state.groupOffsetsStart[id] = *value;
        }
        state.groupMove = state.groupOffsetsStart.size() > 1;
        if (state.groupMove) {
            state.layoutBefore = m_session.DocumentView().CaptureLayout();
            return;
        }
    }

    const Status status = m_session.Commands().BeginPropertyGesture(
        target, "offsets", handle == 0 ? "Move Control" : "Resize Control",
        DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
    if (!status) Report(status);
}

void CanvasInteractionController::CommitManagedDrag(Vec2 canvas, bool detach) {
    auto* document = m_session.Document();
    if (!document) return;
    auto* node = m_session.DocumentView().Find(*document, Selected());
    if (!node) return;
    const Uuid oldParent = node->parent;
    const std::size_t oldIndex =
        m_session.DocumentView().ChildIndex(Selected()).value_or(0);
    const auto policy = SelectedParentPolicy();
    if (detach) {
        const Uuid freeParent = NearestFreeAncestor(Selected());
        if (freeParent.Empty() || freeParent == oldParent) {
            m_session.canvas.hint = "No free-layout ancestor is available";
            return;
        }
        const Rect visual = SelectedRect();
        const auto parentRect = m_session.DocumentView().LayoutRect(freeParent);
        const Rect parent = parentRect
                                ? *parentRect
                                : Rect{0, 0, CanvasSize().x, CanvasSize().y};
        const Rect anchors{0, 0, 0, 0};
        const Rect offsets =
            ui::ControlLayoutMath::OffsetsForRect(parent, anchors, visual);
        const Variant oldAnchors = node->properties.contains("anchors")
                                       ? node->properties.at("anchors")
                                       : Variant{};
        const Variant oldOffsets = node->properties.contains("offsets")
                                       ? node->properties.at("offsets")
                                       : Variant{};
        auto command = std::make_unique<CompositeEditCommand>("Detach and move Control");
        command->Add(std::make_unique<ReparentEditCommand>(
            "Detach Control", Selected(), oldParent, oldIndex, freeParent,
            m_session.DocumentView().Children(freeParent).size()));
        command->Add(std::make_unique<PropertyChangeCommand>(
            "Reset anchors", Selected(), "anchors", oldAnchors, Variant(anchors),
            std::chrono::steady_clock::now(), false));
        command->Add(std::make_unique<PropertyChangeCommand>(
            "Preserve visual position", Selected(), "offsets", oldOffsets,
            Variant(offsets), std::chrono::steady_clock::now(), false));
        DocumentChangeSet changes = DocumentChangeSet::Structure(freeParent);
        changes.Merge(DocumentChangeSet::Property(
            Selected(), "anchors",
            DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint));
        changes.Merge(DocumentChangeSet::Property(
            Selected(), "offsets",
            DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint));
        const Status status =
            m_session.Commands().Execute(std::move(command), std::move(changes));
        if (!status) Report(status);
        else {
            m_session.canvas.hint = "Detached into free layout";
        }
        return;
    }
    if (policy == ui::ChildLayoutPolicy::SingleSlot ||
        policy == ui::ChildLayoutPolicy::RuntimeManaged) {
        m_session.canvas.hint =
            std::string("Position is managed by ") +
            ui::ChildLayoutPolicyName(policy);
        return;
    }
    const std::size_t target = InsertionIndex(Selected(), canvas);
    if (target == oldIndex) return;
    const Status status = m_session.Commands().Reorder(Selected(), target);
    if (!status) Report(status);
}

bool CanvasInteractionController::PointerDown(const DesignerPointerEvent& event) {
    if (m_hasCapture || event.button != DesignerMouseButton::Left ||
        !m_session.Document())
        return false;
    m_hasCapture = HandlePointerDown(event);
    return m_hasCapture;
}

bool CanvasInteractionController::PointerMove(const DesignerPointerEvent& event) {
    return m_hasCapture && HandlePointerMove(event);
}

bool CanvasInteractionController::PointerUp(const DesignerPointerEvent& event) {
    if (!m_hasCapture) return false;
    const bool handled = HandlePointerUp(event);
    m_session.canvas.snapGuides.clear();
    m_session.canvas.snapDistances.clear();
    m_hasCapture = false;
    return handled;
}

bool CanvasInteractionController::HandlePointerDown(
    const DesignerPointerEvent& event) {
    auto* document = m_session.Document();
    if (!document) return false;
    auto& state = m_session.canvas;
    const int resizeHandle = state.hoveredResizeHandle;
    const int anchorHandle = state.hoveredAnchorHandle;
    const bool pivotHandle = state.hoveredPivotHandle;
    Uuid hit = (resizeHandle || anchorHandle || pivotHandle)
                   ? Selected()
                   : HitTest(event.canvasPosition);
    if (event.modifiers.alt && !resizeHandle) {
        const auto hits = HitTestService{}.HitStack(
            *document, m_session.DocumentView(), event.canvasPosition,
            m_session.Selection().Scope());
        if (!hits.empty()) {
            auto current = std::find(hits.begin(), hits.end(), Selected());
            hit = current == hits.end() || ++current == hits.end() ? hits.front()
                                                                   : *current;
        }
    }
    auto& selection = m_session.Selection();
    if (!hit.Empty()) {
        if (event.modifiers.controlOrCommand && !resizeHandle && !anchorHandle &&
            !pivotHandle) {
            if (selection.Contains(hit)) {
                selection.Remove(hit);
                return true;
            }
            selection.Add(hit);
        } else if (!selection.Contains(hit)) {
            selection.Replace(hit);
        }
        if (!selection.SetPrimary(hit)) selection.Replace(hit);
        selection.Canonicalize(m_session.DocumentView());
        hit = Selected();
        state.hint.clear();
        const auto policy = SelectedParentPolicy();
        const auto* selectedNode =
            m_session.DocumentView().Find(*document, hit);
        const bool locked =
            selectedNode && selectedNode->properties.contains("editorLocked") &&
            selectedNode->properties.at("editorLocked").TryGet<bool>() &&
            *selectedNode->properties.at("editorLocked").TryGet<bool>();
        if (locked) {
            state.hint = "This Control is locked in the Scene Tree";
        } else if (pivotHandle) {
            state.pivotStart = {0.5f, 0.5f};
            state.authoredPivotStart = {};
            if (selectedNode) {
                const auto found = selectedNode->properties.find("pivot");
                if (found != selectedNode->properties.end()) {
                    state.authoredPivotStart = found->second.Clone();
                    if (const auto* value = found->second.TryGet<Vec2>())
                        state.pivotStart = *value;
                }
            }
            state.layoutBefore = m_session.DocumentView().CaptureLayout();
            state.dragStart = event.canvasPosition;
            state.dragCurrent = event.canvasPosition;
            state.gesture = DesignerCanvasGesture::Pivot;
            state.gestureDragged = false;
            state.hint = "Drag the transform pivot";
        } else if (anchorHandle) {
            state.anchorsStart = {};
            state.anchorOffsetsStart = {};
            state.authoredAnchorsStart = {};
            state.authoredAnchorOffsetsStart = {};
            if (selectedNode) {
                const auto anchors = selectedNode->properties.find("anchors");
                if (anchors != selectedNode->properties.end()) {
                    state.authoredAnchorsStart = anchors->second.Clone();
                    if (const auto* value = anchors->second.TryGet<Rect>())
                        state.anchorsStart = *value;
                }
                const auto offsets = selectedNode->properties.find("offsets");
                if (offsets != selectedNode->properties.end()) {
                    state.authoredAnchorOffsetsStart = offsets->second.Clone();
                    if (const auto* value = offsets->second.TryGet<Rect>())
                        state.anchorOffsetsStart = *value;
                }
            }
            state.layoutBefore = m_session.DocumentView().CaptureLayout();
            state.rectStart = SelectedRect();
            state.dragStart = event.canvasPosition;
            state.dragCurrent = event.canvasPosition;
            state.anchorHandle = anchorHandle;
            state.gesture = DesignerCanvasGesture::Anchors;
            state.gestureDragged = false;
        } else if (hit != RootId()) {
            if (policy == ui::ChildLayoutPolicy::Free) {
                BeginFreeTransform(hit, event.canvasPosition, resizeHandle);
                state.dragCurrent = event.canvasPosition;
            } else {
                state.gesture = DesignerCanvasGesture::Reorder;
                state.gestureDragged = false;
                state.dragStart = event.canvasPosition;
                state.dragCurrent = event.canvasPosition;
                state.reorderPreview =
                    m_session.DocumentView().ChildIndex(hit).value_or(0);
                state.hint =
                    std::string("Managed by ") + ui::ChildLayoutPolicyName(policy) +
                    "; drag reorders";
            }
        }
    } else {
        state.gesture = DesignerCanvasGesture::Marquee;
        state.dragStart = event.canvasPosition;
        state.dragCurrent = event.canvasPosition;
        state.marqueeCurrent = event.canvasPosition;
        state.marqueeAdditive = event.modifiers.controlOrCommand;
        if (!state.marqueeAdditive) selection.Clear();
    }
    return true;
}

bool CanvasInteractionController::HandlePointerMove(
    const DesignerPointerEvent& event) {
    auto* document = m_session.Document();
    if (!document) return false;
    auto& state = m_session.canvas;
    state.dragCurrent = event.canvasPosition;
    state.snapGuides.clear();
    state.snapDistances.clear();

    if (state.gesture == DesignerCanvasGesture::Anchors) {
        state.gestureDragged = true;
        const Rect parent = ParentRect(Selected());
        Rect anchors = state.anchorsStart;
        float x = parent.w > 0 ? (state.dragCurrent.x - parent.x) / parent.w : 0;
        float y = parent.h > 0 ? (state.dragCurrent.y - parent.y) / parent.h : 0;
        const SnapEngine snap;
        if (m_session.viewport.smartGuides) {
            x = snap.SnapNormalized(x);
            y = snap.SnapNormalized(y);
        } else {
            x = std::clamp(x, 0.0f, 1.0f);
            y = std::clamp(y, 0.0f, 1.0f);
        }
        if (state.anchorHandle == 1 || state.anchorHandle == 4)
            anchors.x = std::min(x, anchors.w);
        else
            anchors.w = std::max(x, anchors.x);
        if (state.anchorHandle == 1 || state.anchorHandle == 2)
            anchors.y = std::min(y, anchors.h);
        else
            anchors.h = std::max(y, anchors.y);
        const Rect offsets =
            ui::ControlLayoutMath::OffsetsForRect(parent, anchors, state.rectStart);
        Status status = m_session.Commands().ApplyTransientProperty(
            Selected(), "anchors", Variant(anchors),
            DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
        if (status)
            status = m_session.Commands().ApplyTransientProperty(
                Selected(), "offsets", Variant(offsets),
                DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
        if (!status) {
            Report(status);
            Cancel();
        }
        return true;
    }

    if (state.gesture == DesignerCanvasGesture::Pivot) {
        state.gestureDragged = true;
        const Rect selected = SelectedRect();
        Vec2 pivot{selected.w > 0
                       ? (state.dragCurrent.x - selected.x) / selected.w
                       : 0.5f,
                   selected.h > 0
                       ? (state.dragCurrent.y - selected.y) / selected.h
                       : 0.5f};
        const SnapEngine snap;
        pivot.x = snap.SnapNormalized(pivot.x);
        pivot.y = snap.SnapNormalized(pivot.y);
        const Status status = m_session.Commands().ApplyTransientProperty(
            Selected(), "pivot", Variant(pivot), DesignerDirtyFlags::Paint);
        if (!status) {
            Report(status);
            Cancel();
        } else {
            state.hint = "Pivot " +
                         std::to_string(static_cast<int>(std::round(pivot.x * 100))) +
                         "%, " +
                         std::to_string(static_cast<int>(std::round(pivot.y * 100))) +
                         "%";
        }
        return true;
    }

    if (state.gesture == DesignerCanvasGesture::Move && state.groupMove) {
        state.gestureDragged = true;
        Vec2 delta{state.dragCurrent.x - state.dragStart.x,
                   state.dragCurrent.y - state.dragStart.y};
        Rect target = state.rectStart;
        target.x += delta.x;
        target.y += delta.y;
        target = SnapRect(target, event.zoom, event.modifiers.alt,
                          m_session.Selection().OrderedItems());
        delta = {target.x - state.rectStart.x, target.y - state.rectStart.y};
        for (const auto& [id, before] : state.groupOffsetsStart) {
            Rect value = before;
            value.x += delta.x;
            value.y += delta.y;
            const Status status = m_session.Commands().ApplyTransientProperty(
                id, "offsets", Variant(value),
                DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
            if (!status) {
                Report(status);
                Cancel();
                return true;
            }
        }
        return true;
    }

    if ((state.gesture == DesignerCanvasGesture::Move ||
         state.gesture == DesignerCanvasGesture::Resize) &&
        m_session.Commands().GestureActive()) {
        state.gestureDragged = true;
        const Vec2 delta{state.dragCurrent.x - state.dragStart.x,
                         state.dragCurrent.y - state.dragStart.y};
        Rect target = state.rectStart;
        if (state.gesture == DesignerCanvasGesture::Move) {
            target.x += delta.x;
            target.y += delta.y;
        } else {
            float left = target.x;
            float top = target.y;
            float right = target.x + target.w;
            float bottom = target.y + target.h;
            if (state.resizeHandle == 1 || state.resizeHandle == 7 ||
                state.resizeHandle == 8)
                left += delta.x;
            if (state.resizeHandle == 3 || state.resizeHandle == 4 ||
                state.resizeHandle == 5)
                right += delta.x;
            if (state.resizeHandle >= 1 && state.resizeHandle <= 3) top += delta.y;
            if (state.resizeHandle >= 5 && state.resizeHandle <= 7)
                bottom += delta.y;
            if (right - left < 8) {
                if (state.resizeHandle == 1 || state.resizeHandle == 7 ||
                    state.resizeHandle == 8)
                    left = right - 8;
                else
                    right = left + 8;
            }
            if (bottom - top < 8) {
                if (state.resizeHandle >= 1 && state.resizeHandle <= 3)
                    top = bottom - 8;
                else
                    bottom = top + 8;
            }
            target = {left, top, right - left, bottom - top};
        }

        if (state.gesture == DesignerCanvasGesture::Resize &&
            (state.resizeHandle == 1 || state.resizeHandle == 3 ||
             state.resizeHandle == 5 || state.resizeHandle == 7)) {
            const auto* selected =
                m_session.DocumentView().Find(*document, Selected());
            bool locked = selected && selected->type == "TextureRect";
            if (selected) {
                const auto found = selected->properties.find("lockAspectRatio");
                if (found != selected->properties.end())
                    if (const auto* value = found->second.TryGet<bool>())
                        locked = *value;
            }
            if (event.modifiers.shift) locked = !locked;
            if (locked) {
                float ratio =
                    state.rectStart.h > 0 ? state.rectStart.w / state.rectStart.h
                                          : 1.0f;
                if (selected && m_imageSizeResolver) {
                    const auto path = selected->properties.find("path");
                    if (path != selected->properties.end())
                        if (const auto* value = path->second.TryGet<std::string>())
                            if (const auto size = m_imageSizeResolver(*value);
                                size && size->y > 0)
                                ratio = size->x / size->y;
                }
                float left = target.x;
                float right = target.x + target.w;
                float top = target.y;
                float bottom = target.y + target.h;
                if (std::abs(delta.x) >= std::abs(delta.y) * ratio) {
                    const float height = target.w / ratio;
                    if (state.resizeHandle == 1 || state.resizeHandle == 3)
                        top = bottom - height;
                    else
                        bottom = top + height;
                } else {
                    const float width = target.h * ratio;
                    if (state.resizeHandle == 1 || state.resizeHandle == 7)
                        left = right - width;
                    else
                        right = left + width;
                }
                target = {left, top, std::max(8.0f, right - left),
                          std::max(8.0f, bottom - top)};
            }
        }

        const std::array<Uuid, 1> ignored{Selected()};
        target = SnapRect(target, event.zoom, event.modifiers.alt, ignored);
        target.w = std::max(8.0f, target.w);
        target.h = std::max(8.0f, target.h);
        if (state.gesture == DesignerCanvasGesture::Resize)
            state.hint =
                std::to_string(static_cast<int>(std::round(target.w))) + " x " +
                std::to_string(static_cast<int>(std::round(target.h)));
        Rect anchors{};
        const auto* node = m_session.DocumentView().Find(*document, Selected());
        if (node) {
            const auto found = node->properties.find("anchors");
            if (found != node->properties.end())
                if (const auto* value = found->second.TryGet<Rect>()) anchors = *value;
        }
        const Rect offsets = ui::ControlLayoutMath::OffsetsForRect(
            ParentRect(Selected()), anchors, target);
        const Status status =
            m_session.Commands().UpdatePropertyGesture(Variant(offsets));
        if (!status) Report(status);
        return true;
    }

    if (state.gesture == DesignerCanvasGesture::Reorder) {
        state.gestureDragged = true;
        state.reorderPreview = InsertionIndex(Selected(), state.dragCurrent);
        return true;
    }
    if (state.gesture == DesignerCanvasGesture::Marquee) {
        state.gestureDragged = true;
        state.marqueeCurrent = state.dragCurrent;
        return true;
    }
    return false;
}

bool CanvasInteractionController::HandlePointerUp(
    const DesignerPointerEvent& event) {
    auto* document = m_session.Document();
    if (!document) return false;
    auto& state = m_session.canvas;
    if (state.gesture == DesignerCanvasGesture::Marquee) {
        const float left = std::min(state.dragStart.x, state.marqueeCurrent.x);
        const float right = std::max(state.dragStart.x, state.marqueeCurrent.x);
        const float top = std::min(state.dragStart.y, state.marqueeCurrent.y);
        const float bottom = std::max(state.dragStart.y, state.marqueeCurrent.y);
        for (const auto& node : document->Data().nodes) {
            if (node.id == RootId() ||
                !m_session.DocumentView().IsWithin(m_session.Selection().Scope(),
                                                   node.id))
                continue;
            const auto locked = node.properties.find("editorLocked");
            if (locked != node.properties.end())
                if (const auto* value = locked->second.TryGet<bool>(); value && *value)
                    continue;
            const auto visibility = node.properties.find("visibility");
            if (visibility != node.properties.end())
                if (const auto* value = visibility->second.TryGet<std::string>();
                    value && *value != "Visible")
                    continue;
            const auto rect = m_session.DocumentView().LayoutRect(node.id);
            if (rect && rect->x <= right && rect->x + rect->w >= left &&
                rect->y <= bottom && rect->y + rect->h >= top)
                m_session.Selection().Add(node.id);
        }
        m_session.Selection().Canonicalize(m_session.DocumentView());
        state.gesture = DesignerCanvasGesture::None;
        state.gestureDragged = false;
        return true;
    }

    if (state.gesture == DesignerCanvasGesture::Anchors) {
        if (state.gestureDragged) {
            auto anchors = document->ReadProperty(Selected(), "anchors");
            auto offsets = document->ReadProperty(Selected(), "offsets");
            auto command = std::make_unique<CompositeEditCommand>("Adjust anchors");
            if (anchors)
                command->Add(std::make_unique<PropertyChangeCommand>(
                    "Anchors", Selected(), "anchors", state.authoredAnchorsStart.Clone(),
                    anchors.Value()));
            if (offsets)
                command->Add(std::make_unique<PropertyChangeCommand>(
                    "Preserve position", Selected(), "offsets",
                    state.authoredAnchorOffsetsStart.Clone(), offsets.Value()));
            DocumentChangeSet changes = DocumentChangeSet::Property(
                Selected(), "anchors",
                DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
            changes.Merge(DocumentChangeSet::Property(
                Selected(), "offsets",
                DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint));
            const Status status =
                m_session.Commands().CommitApplied(std::move(command), std::move(changes));
            if (!status) Report(status);
        }
        state.anchorHandle = 0;
        state.layoutBefore.reset();
        state.gesture = DesignerCanvasGesture::None;
        state.gestureDragged = false;
        return true;
    }

    if (state.gesture == DesignerCanvasGesture::Pivot) {
        if (state.gestureDragged) {
            auto pivot = document->ReadProperty(Selected(), "pivot");
            if (pivot) {
                auto command = std::make_unique<PropertyChangeCommand>(
                    "Adjust pivot", Selected(), "pivot", state.authoredPivotStart.Clone(),
                    pivot.Value(), std::chrono::steady_clock::now(), false);
                const Status status = m_session.Commands().CommitApplied(
                    std::move(command),
                    DocumentChangeSet::Property(Selected(), "pivot",
                                                DesignerDirtyFlags::Paint));
                if (!status) Report(status);
            }
        }
        state.layoutBefore.reset();
        state.gesture = DesignerCanvasGesture::None;
        state.gestureDragged = false;
        return true;
    }

    if (state.gesture == DesignerCanvasGesture::Move && state.groupMove) {
        if (state.gestureDragged) {
            auto command =
                std::make_unique<CompositeEditCommand>("Move multiple Controls");
            DocumentChangeSet changes;
            for (const auto& [id, before] : state.groupOffsetsStart) {
                auto after = document->ReadProperty(id, "offsets");
                if (!after) continue;
                command->Add(std::make_unique<PropertyChangeCommand>(
                    "Move Control", id, "offsets", Variant(before), after.Value()));
                changes.Merge(DocumentChangeSet::Property(
                    id, "offsets",
                    DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint));
            }
            const Status status =
                m_session.Commands().CommitApplied(std::move(command), std::move(changes));
            if (!status) Report(status);
        }
        state.groupOffsetsStart.clear();
        state.groupMove = false;
        state.layoutBefore.reset();
        state.gesture = DesignerCanvasGesture::None;
        state.gestureDragged = false;
        return true;
    }

    if (state.gesture == DesignerCanvasGesture::Move ||
        state.gesture == DesignerCanvasGesture::Resize) {
        if (m_session.Commands().GestureActive()) {
            const Status status = m_session.Commands().CommitPropertyGesture();
            if (!status) Report(status);
        }
        state.gesture = DesignerCanvasGesture::None;
        state.gestureDragged = false;
        return true;
    }

    if (state.gesture == DesignerCanvasGesture::Reorder) {
        if (state.gestureDragged)
            CommitManagedDrag(event.canvasPosition,
                              event.modifiers.controlOrCommand);
        state.gesture = DesignerCanvasGesture::None;
        state.gestureDragged = false;
        return true;
    }
    // PointerDown accepted the interaction even when it was a selection-only
    // click with no gesture state to commit.
    return true;
}

void CanvasInteractionController::Cancel() {
    auto* document = m_session.Document();
    auto& state = m_session.canvas;
    if (state.gesture == DesignerCanvasGesture::Anchors && document) {
        (void)m_session.Commands().ApplyTransientProperty(
            Selected(), "anchors", state.authoredAnchorsStart,
            DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
        (void)m_session.Commands().ApplyTransientProperty(
            Selected(), "offsets", state.authoredAnchorOffsetsStart,
            DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
    }
    if (state.gesture == DesignerCanvasGesture::Pivot && document)
        (void)m_session.Commands().ApplyTransientProperty(
            Selected(), "pivot", state.authoredPivotStart,
            DesignerDirtyFlags::Paint);
    if (state.groupMove && document) {
        for (const auto& [id, value] : state.groupOffsetsStart)
            (void)m_session.Commands().ApplyTransientProperty(
                id, "offsets", Variant(value),
                DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
    }
    if (m_session.Commands().GestureActive()) {
        const Status status = m_session.Commands().CancelPropertyGesture();
        if (!status) Report(status);
    }
    if (state.layoutBefore)
        m_session.DocumentView().RestoreLayout(std::move(*state.layoutBefore));
    state.layoutBefore.reset();
    state.groupOffsetsStart.clear();
    state.groupMove = false;
    state.gesture = DesignerCanvasGesture::None;
    state.gestureDragged = false;
    state.resizeHandle = 0;
    state.anchorHandle = 0;
    state.snapGuides.clear();
    state.snapDistances.clear();
    m_hasCapture = false;
}

}  // namespace px::editor
