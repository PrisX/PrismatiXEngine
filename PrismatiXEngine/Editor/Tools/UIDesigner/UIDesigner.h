#pragma once

#include "Editor/Tools/UIDesigner/UISceneDocument.h"
#include "Editor/Tools/UIDesigner/UIDesignerSession.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/UI/Binding.h"
#include "Editor/Tools/UIDesigner/Preview/PreviewFixture.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <functional>
#include <chrono>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace px::editor {

class BehaviorGraphEditor;
class AnimationStateMachineEditor;

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
    bool leftPanelVisible = true;
    bool rightPanelVisible = true;
    bool bottomPanelVisible = false;
    float leftPanelWidth = 260.0f;
    float rightPanelWidth = 340.0f;
    float bottomPanelHeight = 240.0f;
    int authorMode = 0;       // Design, Interact, Animate.
    int animateSurface = 0;   // Clip, State Machine.
};

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
    void SetOnEdit(std::function<void()> cb) { m_onEdit = std::move(cb); }
    void SetOnStructureChange(std::function<void()> cb) { m_onStructure = std::move(cb); }
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
    bool HandleCanvasInteraction(const ImRect& viewport, ImVec2 p0, float scale, bool hovered,
                                 const std::string& selectedAssetPath);
    void RenderCanvasOverlay(ImVec2 p0, float scale);
    void AddImageAt(float canvasX, float canvasY, const std::string& image);

    bool Save();
    [[nodiscard]] bool Dirty() const { return m_document && m_document->History().Dirty(); }
    [[nodiscard]] const std::string& Path() const { return m_pathText; }
    [[nodiscard]] UISceneDocument* Document() const { return m_document; }
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
    static void RegenerateIds(VariantObject& subtree);
    void RecordAnimationKey(const Uuid& node, const std::string& property, const Variant& value);

    std::unique_ptr<UIDesignerSession> m_session;
    std::unique_ptr<BehaviorGraphEditor> m_behaviorEditor;
    std::unique_ptr<AnimationStateMachineEditor> m_animationStateEditor;
    UISceneDocument* m_document = nullptr;
    std::string m_pathText;
    Uuid m_selected;
    std::unordered_set<Uuid, UuidHash> m_selection;
    Uuid m_hovered;
    std::unordered_map<Uuid, Rect, UuidHash> m_layout;
    std::unordered_map<Uuid, ui::ChildLayoutPolicy, UuidHash> m_childPolicies;
    std::unique_ptr<PropertyEditTransaction> m_propertyTransaction;
    std::unique_ptr<MultiPropertyEditTransaction> m_multiPropertyTransaction;
    std::string m_multiTransactionProperty;
    std::string m_transactionProperty;
    Uuid m_transactionTarget;
    enum class Gesture { None, Move, Resize, Anchors, Pivot, Reorder, Marquee };
    Gesture m_gesture = Gesture::None;
    bool m_gestureDragged = false;
    int m_resizeHandle = 0;
    int m_anchorHandle = 0;
    Rect m_anchorsStart{};
    Rect m_anchorOffsetsStart{};
    Vec2 m_pivotStart{.5f,.5f};
    Vec2 m_dragStart{};
    Vec2 m_dragCurrent{};
    float m_dragScale = 1.0f;
    Vec2 m_marqueeCurrent{};
    bool m_marqueeAdditive = false;
    Rect m_rectStart{};
    Rect m_offsetsStart{};
    std::unordered_map<Uuid, Rect, UuidHash> m_groupOffsetsStart;
    bool m_groupMove = false;
    int m_gridSize = 16;
    std::size_t m_reorderPreview = 0;
    float m_guideX = std::numeric_limits<float>::quiet_NaN();
    float m_guideY = std::numeric_limits<float>::quiet_NaN();
    std::string m_canvasHint;
    VariantObject m_clipboardSubtree;
    Vec2 m_contextCanvas{};
    Uuid m_contextTarget;
    char m_treeFilter[96] = {0};
    char m_paletteFilter[96] = {0};
    char m_actionFilter[96] = {0};
    char m_treeRename[128] = {0};
    bool m_treeRenameOpen = false;
    bool m_createComponentOpen = false;
    char m_componentPath[260] = "Content/UI/Components/NewComponent.pxcomponent";
    DesignerViewportState m_viewport;
    std::function<void()> m_onEdit;
    std::function<void()> m_onStructure;
    ComponentWriter m_componentWriter;
    std::function<void(const ResourceRefValue&)> m_openResource;
    ImageSizeResolver m_imageSizeResolver;
    IdentityRegistrar m_identityRegistrar;
    ui::FormatterRegistry m_formatters;
    PreviewFixture m_previewFixture;
    Uuid m_selectedClip;
    float m_timelineTime = 0.0f;
    bool m_timelinePlaying = false;
    bool m_timelineAutoKey = false;
    int m_timelineTrack = -1;
    int m_timelineKey = -1;
    std::string m_timelineProperty = "offsets";
    std::string m_selectedSignal;
    AnimationPreview m_animationPreview;
    BehaviorDebugProvider m_behaviorDebugProvider;
    AnimationDebugProvider m_animationDebugProvider;
    std::function<void(int)> m_requestAuthorMode;
    std::chrono::steady_clock::time_point m_lastEdit = std::chrono::steady_clock::now();
};

struct DesignerDocumentSession {
    std::filesystem::path canonicalPath;
    std::unique_ptr<UIDesigner> editor;
};

}  // namespace px::editor
