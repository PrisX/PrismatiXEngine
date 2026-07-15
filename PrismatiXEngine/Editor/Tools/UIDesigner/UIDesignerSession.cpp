#include "Editor/Tools/UIDesigner/UIDesignerSession.h"

#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"
#include "Editor/Tools/UIDesigner/Canvas/DesignerTools.h"
#include "Editor/Tools/UIDesigner/DesignerCommandService.h"

namespace px::editor {

UIDesignerSession::UIDesignerSession()
    : m_interaction(std::make_unique<CanvasInteractionController>()),
      m_selectTool(std::make_unique<SelectTool>()),
      m_anchorTool(std::make_unique<AnchorTool>()),
      m_commands(std::make_unique<DesignerCommandService>(*this)) {
    m_commands->SetChanged([this](const DocumentChangeSet& changes) {
        if (m_changeListener) m_changeListener(changes);
    });
}

UIDesignerSession::~UIDesignerSession() = default;

Status UIDesignerSession::Open(const std::filesystem::path& path) {
    auto document = std::make_unique<UISceneDocument>();
    const Status loaded = document->Load(path);
    return loaded ? Adopt(std::move(document)) : loaded;
}

Status UIDesignerSession::New(const std::filesystem::path& path, int width, int height) {
    auto document = std::make_unique<UISceneDocument>();
    const Status created = document->New(path, width, height);
    return created ? Adopt(std::move(document)) : created;
}

void UIDesignerSession::Close() {
    if (m_interaction) m_interaction->Cancel();
    m_document.reset();
    m_documentView.Clear();
    m_selection.Clear();
    m_selection.ClearScope();
    ResetViewState();
    m_dirtyFlags = DesignerDirtyFlags::None;
}

CanvasInteractionController& UIDesignerSession::Interaction() { return *m_interaction; }
const CanvasInteractionController& UIDesignerSession::Interaction() const { return *m_interaction; }
IDesignerTool& UIDesignerSession::CanvasTool(DesignerTool tool) {
    return tool == DesignerTool::Anchors ? static_cast<IDesignerTool&>(*m_anchorTool)
                                         : static_cast<IDesignerTool&>(*m_selectTool);
}
DesignerCommandService& UIDesignerSession::Commands() { return *m_commands; }
const DesignerCommandService& UIDesignerSession::Commands() const { return *m_commands; }
void UIDesignerSession::SetChangeListener(ChangeListener listener) {
    m_changeListener = std::move(listener);
}
bool UIDesignerSession::HistoryDirty() const { return m_commands->HistoryDirty(); }
std::size_t UIDesignerSession::HistoryCursor() const { return m_commands->HistoryCursor(); }

DesignerDirtyFlags UIDesignerSession::ConsumeDirtyFlags() {
    const DesignerDirtyFlags result = m_dirtyFlags;
    m_dirtyFlags = DesignerDirtyFlags::None;
    return result;
}

Status UIDesignerSession::Adopt(std::unique_ptr<UISceneDocument> document) {
    DesignerDocumentView view;
    const Status indexed = view.Rebuild(*document);
    if (!indexed) return indexed;
    if (m_interaction) m_interaction->Cancel();
    m_document = std::move(document);
    m_documentView = std::move(view);
    ResetViewState();
    if (!m_documentView.Root().Empty()) {
        m_selection.Replace(m_documentView.Root());
        expandedTreeNodes.insert(m_documentView.Root());
    }
    m_dirtyFlags = kAllDesignerContentDirty | DesignerDirtyFlags::PreviewState;
    return Status::Ok();
}

void UIDesignerSession::ResetViewState() {
    m_selection.Clear();
    m_selection.ClearScope();
    hoveredNode = {};
    expandedTreeNodes.clear();
    inspectorSearch.clear();
    viewport = {};
    timeline = {};
    behaviorGraph = {};
    animationMachine = {};
    canvas = {};
    panels = {};
    previewFixture = {};
    clipboardSubtree.clear();
    selectedSignal.clear();
    lastEdit = std::chrono::steady_clock::now();
}

}  // namespace px::editor
