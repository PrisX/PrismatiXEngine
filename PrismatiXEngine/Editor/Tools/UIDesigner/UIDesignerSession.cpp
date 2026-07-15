#include "Editor/Tools/UIDesigner/UIDesignerSession.h"

#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"
#include "Editor/Tools/UIDesigner/DesignerCommandService.h"

namespace px::editor {

UIDesignerSession::UIDesignerSession()
    : m_interaction(std::make_unique<CanvasInteractionController>(*this)),
      m_commands(std::make_unique<DesignerCommandService>(*this)) {
    m_commands->SetChanged([this](const DocumentChangeSet& changes) {
        m_pendingChanges.Merge(changes);
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
    // Property transactions retain references to both the document and its
    // EditHistory. They must be destroyed before the document.
    if (m_commands) (void)m_commands->ResetForDocumentChange();
    m_document.reset();
    m_documentView.Clear();
    m_selection.Clear();
    m_selection.ClearScope();
    ResetViewState();
    m_dirtyFlags = DesignerDirtyFlags::None;
    m_pendingChanges = {};
}

CanvasInteractionController& UIDesignerSession::Interaction() { return *m_interaction; }
const CanvasInteractionController& UIDesignerSession::Interaction() const { return *m_interaction; }
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

DocumentChangeSet UIDesignerSession::ConsumeDocumentChanges() {
    DocumentChangeSet result = std::move(m_pendingChanges);
    result.dirty |= m_dirtyFlags;
    m_pendingChanges = {};
    m_dirtyFlags = DesignerDirtyFlags::None;
    return result;
}

Status UIDesignerSession::Adopt(std::unique_ptr<UISceneDocument> document) {
    DesignerDocumentView view;
    const Status indexed = view.Rebuild(*document);
    if (!indexed) return indexed;
    if (m_interaction) m_interaction->Cancel();
    if (m_commands) {
        const Status shutdown = m_commands->ResetForDocumentChange();
        if (!shutdown) return shutdown;
    }
    m_document = std::move(document);
    m_documentView = std::move(view);
    ResetViewState();
    if (!m_documentView.Root().Empty()) {
        m_selection.Replace(m_documentView.Root());
        expandedTreeNodes.insert(m_documentView.Root());
    }
    m_dirtyFlags = kAllDesignerContentDirty | DesignerDirtyFlags::PreviewState;
    m_pendingChanges = DocumentChangeSet::WholeDocument(kAllDesignerContentDirty);
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
