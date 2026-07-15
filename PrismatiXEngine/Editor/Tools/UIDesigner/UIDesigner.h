#pragma once

#include "Editor/Tools/UIDesigner/UISceneDocument.h"
#include "Editor/Tools/UIDesigner/UIDesignerSession.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/UI/Binding.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <functional>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace px::editor {

class BehaviorGraphEditor;
class AnimationStateMachineEditor;
struct DesignerPointerEvent;

class UIDesigner {
public:
    UIDesigner();
    ~UIDesigner();
    UIDesigner(const UIDesigner&) = delete;
    UIDesigner& operator=(const UIDesigner&) = delete;
    UIDesigner(UIDesigner&&) noexcept;
    UIDesigner& operator=(UIDesigner&& other) noexcept;

    Status Open(const std::filesystem::path& path);
    Status New(const std::filesystem::path& path, int width = 1280, int height = 720);
    void SetOnDocumentChange(UIDesignerSession::ChangeListener listener) {
        m_session->SetChangeListener(std::move(listener));
    }
    [[nodiscard]] UIDesignerSession* SessionIdentity() { return m_session.get(); }
    [[nodiscard]] const UIDesignerSession* SessionIdentity() const { return m_session.get(); }
    using ComponentWriter=std::function<Result<ResourceRefValue>(const std::filesystem::path&,const std::string&)>;
    void SetComponentWriter(ComponentWriter writer){m_componentWriter=std::move(writer);}
    void SetOpenResource(std::function<void(const ResourceRefValue&)> callback){m_openResource=std::move(callback);}
    using ImageSizeResolver=std::function<std::optional<Vec2>(const std::string&)>;
    void SetImageSizeResolver(ImageSizeResolver resolver){m_imageSizeResolver=std::move(resolver);}
    using IdentityRegistrar=std::function<Status(const std::filesystem::path&)>;
    void SetIdentityRegistrar(IdentityRegistrar registrar){m_identityRegistrar=std::move(registrar);}
    using AnimationPreview=std::function<Status(const Uuid&,float,bool)>;
    void SetAnimationPreview(AnimationPreview preview){m_animationPreview=std::move(preview);}
    using BehaviorDebugProvider=std::function<ui::BehaviorRuntimeState()>;
    void SetBehaviorDebugProvider(BehaviorDebugProvider provider){m_behaviorDebugProvider=std::move(provider);}
    using AnimationDebugProvider=std::function<ui::UIAnimationRuntimeState()>;
    void SetAnimationDebugProvider(AnimationDebugProvider provider){m_animationDebugProvider=std::move(provider);}
    void SetAnimationParameterTester(std::function<Status(std::string_view,const Variant&)> tester);
    void SetRequestAuthorMode(std::function<void(int)> request){m_requestAuthorMode=std::move(request);}

    void RenderHierarchy();
    void RenderInsert();
    void RenderInspector(const std::string& selectedAssetPath);
    void RenderAnimation();
    void RenderAnimationNavigator();
    void RenderAnimationStateMachine();
    void RenderAnimationInspector();
    void RenderTheme();
    void RenderComponents();
    void RenderEvents();
    void RenderInteractionNavigator();
    void RenderInteractionInspector();
    void RenderBehaviorGraph();
    void RenderBehaviorInspector();
    void RenderProblems();
    void RenderViewportToolbar();
    bool ProcessCanvasInput(const ImRect& viewport, ImVec2 p0, float scale, bool hovered,
                            const std::string& selectedAssetPath);
    void RenderCanvasOverlay(ImVec2 p0, float scale);
    void AddImageAt(float canvasX, float canvasY, const std::string& image);

    bool Save();
    [[nodiscard]] bool Dirty() const;
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    [[nodiscard]] std::string NextUndoLabel() const;
    [[nodiscard]] std::string NextRedoLabel() const;
    [[nodiscard]] std::size_t HistoryCursor() const;
    [[nodiscard]] UISceneDocument* Document() const {
        return m_session ? m_session->Document() : nullptr;
    }
    [[nodiscard]] bool HasDocument() const { return Document() != nullptr; }
    [[nodiscard]] std::chrono::steady_clock::time_point LastEditTime() const { return m_session->lastEdit; }
    [[nodiscard]] const DesignerViewportState& ViewportState() const { return m_session->viewport; }
    [[nodiscard]] DesignerViewportState& ViewportState() { return m_session->viewport; }
    [[nodiscard]] Vec2 CanvasSize() const;
    [[nodiscard]] std::string SelectionSummary() const;
    [[nodiscard]] ui::ChildLayoutPolicy SelectedParentPolicy() const;
    Status Undo();
    Status Redo();
    void RelocateDocument(const std::filesystem::path& oldPath,
                          const std::filesystem::path& newPath);
    enum class LayerAction { BringForward, SendBackward, BringToFront, SendToBack };
    enum class AlignAction { Left, HCenter, Right, Top, VCenter, Bottom, DistributeH, DistributeV };
    void ChangeSelectedLayer(LayerAction action);
    void AlignSelection(AlignAction action);
    Status CreateComponentFromSelected(const std::filesystem::path& path);

private:
    void RenderTreeNode(resource::NodeRecord& record);
    [[nodiscard]] bool TreeMatches(const resource::NodeRecord& record) const;
    void RebuildLayout();
    void AddNode(std::string type, Vec2 canvasPosition = {}, std::string image = {});
    void RenderAddControlPalette(Vec2 canvasPosition = {});
    void RemoveSelected();
    void DuplicateSelected();
    void SetSelectedAsBackground(bool lock);
    void RestoreSelectedImageSize();
    void CopySelected();
    void PasteClipboard(Vec2 canvasPosition = {});
    void ResetComponentOverride(const std::string& sourceNode = {}, const std::string& property = {});
    void DetachSelectedComponent();
    [[nodiscard]] Result<resource::TypedDocument> LoadReferencedUI(const ResourceRefValue& reference) const;
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
    bool HandleCanvasPointerDown(const DesignerPointerEvent& event);
    bool HandleCanvasPointerMove(const DesignerPointerEvent& event);
    bool HandleCanvasPointerUp(const DesignerPointerEvent& event);
    static void RegenerateIds(VariantObject& subtree);
    void RecordAnimationKey(const Uuid& node, const std::string& property, const Variant& value);
    [[nodiscard]] DesignerSelection& Selection() { return m_session->Selection(); }
    [[nodiscard]] const DesignerSelection& Selection() const { return m_session->Selection(); }
    [[nodiscard]] Uuid Selected() const { return Selection().Primary(); }
    void MakePrimary(const Uuid& id) {
        if (!Selection().SetPrimary(id)) Selection().Replace(id);
    }
    void SelectOnly(const Uuid& id) { Selection().Replace(id); }
    [[nodiscard]] DesignerDocumentView& View() { return m_session->DocumentView(); }
    [[nodiscard]] const DesignerDocumentView& View() const { return m_session->DocumentView(); }

    std::unique_ptr<UIDesignerSession> m_session;
    std::unique_ptr<BehaviorGraphEditor> m_behaviorEditor;
    std::unique_ptr<AnimationStateMachineEditor> m_animationStateEditor;
    ComponentWriter m_componentWriter;
    std::function<void(const ResourceRefValue&)> m_openResource;
    ImageSizeResolver m_imageSizeResolver;
    IdentityRegistrar m_identityRegistrar;
    ui::FormatterRegistry m_formatters;
    AnimationPreview m_animationPreview;
    BehaviorDebugProvider m_behaviorDebugProvider;
    AnimationDebugProvider m_animationDebugProvider;
    std::function<void(int)> m_requestAuthorMode;
};

struct DesignerDocumentSession {
    std::filesystem::path canonicalPath;
    std::unique_ptr<UIDesigner> editor;
};

}  // namespace px::editor
