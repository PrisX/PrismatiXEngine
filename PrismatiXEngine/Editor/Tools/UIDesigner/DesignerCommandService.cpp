#include "Editor/Tools/UIDesigner/DesignerCommandService.h"

#include <algorithm>
#include <unordered_set>

namespace px::editor {
namespace {

diag::Diagnostic Error(std::string code, std::string message, const Uuid& node = {}) {
    diag::Diagnostic result{.severity = diag::Severity::Error,
                            .code = std::move(code),
                            .category = "Editor.UIDesigner.Command",
                            .message = std::move(message)};
    if (!node.Empty()) result.source.nodeId = node.ToString();
    return result;
}

}  // namespace

Status DesignerCommandService::After(const DocumentChangeSet& changes) {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    if (changes.Structural()) {
        const Status rebuilt = m_session.DocumentView().Rebuild(*document);
        if (!rebuilt) return rebuilt;
        m_session.Selection().Canonicalize(m_session.DocumentView());
    }
    m_session.MarkDirty(changes.dirty);
    if (m_changed) m_changed(changes);
    return Status::Ok();
}

void DesignerCommandService::RecordHistoryChange(std::size_t beforeCursor,
                                                 std::size_t afterCursor,
                                                 const DocumentChangeSet& changes) {
    if (m_historyChanges.size() > beforeCursor) m_historyChanges.resize(beforeCursor);
    if (afterCursor == beforeCursor && beforeCursor > 0) {
        if (m_historyChanges.size() < beforeCursor)
            m_historyChanges.resize(beforeCursor,
                DocumentChangeSet::WholeDocument(kAllDesignerContentDirty));
        m_historyChanges[beforeCursor - 1].Merge(changes);
        return;
    }
    if (afterCursor == beforeCursor + 1) {
        if (m_historyChanges.size() < beforeCursor)
            m_historyChanges.resize(beforeCursor,
                DocumentChangeSet::WholeDocument(kAllDesignerContentDirty));
        m_historyChanges.push_back(changes);
    }
}

Status DesignerCommandService::Execute(std::unique_ptr<EditCommand> command,
                                       DocumentChangeSet changes) {
    auto* document = m_session.Document();
    if (!document || !command)
        return Status::Fail(Error("PXEDCMD6001", "No UI document or command is active"));
    const std::size_t beforeCursor = document->History().Cursor();
    const Status status = document->History().Execute(std::move(command));
    if (!status) return status;
    RecordHistoryChange(beforeCursor, document->History().Cursor(), changes);
    return After(changes);
}

Status DesignerCommandService::Execute(std::unique_ptr<EditCommand> command,
                                       DesignerDirtyFlags dirty, bool structural) {
    return Execute(std::move(command), structural ? DocumentChangeSet::Structure()
                                                  : DocumentChangeSet::WholeDocument(dirty));
}

Status DesignerCommandService::CommitApplied(std::unique_ptr<EditCommand> command,
                                             DocumentChangeSet changes) {
    auto* document = m_session.Document();
    if (!document || !command)
        return Status::Fail(Error("PXEDCMD6001", "No UI document or command is active"));
    const std::size_t beforeCursor = document->History().Cursor();
    const Status status = document->History().CommitApplied(std::move(command));
    if (!status) return status;
    RecordHistoryChange(beforeCursor, document->History().Cursor(), changes);
    return After(changes);
}

Status DesignerCommandService::CommitApplied(std::unique_ptr<EditCommand> command,
                                             DesignerDirtyFlags dirty, bool structural) {
    return CommitApplied(std::move(command), structural ? DocumentChangeSet::Structure()
                                                        : DocumentChangeSet::WholeDocument(dirty));
}

Status DesignerCommandService::Undo() {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    CancelPropertyGesture();
    const std::size_t beforeCursor = document->History().Cursor();
    DocumentChangeSet changes = beforeCursor > 0 && beforeCursor <= m_historyChanges.size()
                                    ? m_historyChanges[beforeCursor - 1]
                                    : DocumentChangeSet::WholeDocument(kAllDesignerContentDirty);
    changes.historyNavigation = true;
    const Status status = document->History().Undo();
    return status ? After(changes) : status;
}

Status DesignerCommandService::Redo() {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    CancelPropertyGesture();
    const std::size_t beforeCursor = document->History().Cursor();
    DocumentChangeSet changes = beforeCursor < m_historyChanges.size()
                                    ? m_historyChanges[beforeCursor]
                                    : DocumentChangeSet::WholeDocument(kAllDesignerContentDirty);
    changes.historyNavigation = true;
    const Status status = document->History().Redo();
    return status ? After(changes) : status;
}

bool DesignerCommandService::CanUndo() const {
    const auto* document = m_session.Document();
    return document && document->History().CanUndo();
}

bool DesignerCommandService::CanRedo() const {
    const auto* document = m_session.Document();
    return document && document->History().CanRedo();
}

std::string DesignerCommandService::NextUndoLabel() const {
    const auto* document = m_session.Document();
    return document ? document->History().NextUndoLabel() : std::string{};
}

std::string DesignerCommandService::NextRedoLabel() const {
    const auto* document = m_session.Document();
    return document ? document->History().NextRedoLabel() : std::string{};
}

std::size_t DesignerCommandService::HistoryCursor() const {
    const auto* document = m_session.Document();
    return document ? document->History().Cursor() : 0;
}

bool DesignerCommandService::HistoryDirty() const {
    const auto* document = m_session.Document();
    return document && document->History().Dirty();
}

Status DesignerCommandService::SetProperty(const Uuid& target, std::string property,
                                           Variant value, std::string label,
                                           DesignerDirtyFlags dirty) {
    auto* document = m_session.Document();
    if (!document) return Status::Fail(Error("PXEDCMD6002", "No UI document is active"));
    auto before = document->ReadProperty(target, property);
    if (!before) return Status::Fail(before.Diagnostics());
    if (before.Value() == value) return Status::Ok();
    if (label.empty()) label = "Change " + property;
    auto changes = DocumentChangeSet::Property(target, property, dirty);
    return Execute(std::make_unique<PropertyChangeCommand>(
                       std::move(label), target, std::move(property), before.TakeValue(),
                       std::move(value)),
                   std::move(changes));
}

Status DesignerCommandService::SetProperties(std::span<const Uuid> targets,
                                             const std::string& property,
                                             const Variant& value, std::string label,
                                             DesignerDirtyFlags dirty) {
    auto* document = m_session.Document();
    if (!document) return Status::Fail(Error("PXEDCMD6002", "No UI document is active"));
    auto composite = std::make_unique<CompositeEditCommand>(std::move(label));
    DocumentChangeSet changes;
    for (const Uuid& target : targets) {
        auto before = document->ReadProperty(target, property);
        if (!before || before.Value() == value) continue;
        composite->Add(std::make_unique<PropertyChangeCommand>(
            "Change " + property, target, property, before.TakeValue(), value.Clone()));
        changes.Merge(DocumentChangeSet::Property(target, property, dirty));
    }
    if (composite->Empty()) return Status::Ok();
    return Execute(std::move(composite), std::move(changes));
}

Status DesignerCommandService::ApplyTransientProperty(const Uuid& target,
                                                       std::string property,
                                                       const Variant& value,
                                                       DesignerDirtyFlags dirty) {
    auto* document = m_session.Document();
    if (!document) return Status::Fail(Error("PXEDCMD6002", "No UI document is active"));
    const Status status = document->WriteProperty(target, property, value);
    if (!status) return status;
    return After(DocumentChangeSet::Property(target, std::move(property), dirty));
}

Status DesignerCommandService::Rename(const Uuid& target, std::string name) {
    return SetProperty(target, "$name", Variant(std::move(name)), "Rename Control",
                       DesignerDirtyFlags::Paint);
}

Status DesignerCommandService::SetVisibility(const Uuid& target, std::string visibility) {
    if (visibility != "Visible" && visibility != "Hidden" && visibility != "Collapsed")
        return Status::Fail(Error("PXEDCMD6003", "Invalid Control visibility", target));
    return SetProperty(target, "visibility", Variant(std::move(visibility)),
                       "Change Visibility", DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
}

Status DesignerCommandService::SetLocked(const Uuid& target, bool locked) {
    return SetProperty(target, "editorLocked", Variant(locked),
                       locked ? "Lock Control" : "Unlock Control", DesignerDirtyFlags::Paint);
}

void DesignerCommandService::RegenerateIds(VariantObject& subtree,
                                           std::vector<Uuid>* generated) {
    const Uuid id = Uuid::Random();
    subtree["id"] = id;
    if (generated) generated->push_back(id);
    if (auto* children = subtree["children"].AsArray())
        for (auto& child : *children)
            if (auto* object = child.AsObject()) RegenerateIds(*object, generated);
}

Status DesignerCommandService::DeleteSelection() {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    m_session.Selection().Canonicalize(m_session.DocumentView());
    auto composite = std::make_unique<CompositeEditCommand>("Delete Controls");
    Uuid fallback = m_session.DocumentView().Root();
    for (const Uuid& id : m_session.Selection().OrderedItems()) {
        if (id == m_session.DocumentView().Root()) continue;
        const auto* node = m_session.DocumentView().Find(*document, id);
        if (!node) continue;
        fallback = node->parent;
        auto captured = document->CaptureSubtree(id);
        if (!captured) return Status::Fail(captured.Diagnostics());
        composite->Add(std::make_unique<SubtreeEditCommand>(
            "Delete " + node->name, SubtreeOperation::Remove, id, node->parent,
            document->ChildIndex(id), captured.TakeValue()));
    }
    if (composite->Empty()) return Status::Ok();
    const Status status = Execute(std::move(composite), DocumentChangeSet::Structure(fallback));
    if (status) m_session.Selection().Replace(fallback);
    return status;
}

Status DesignerCommandService::DuplicateSelection() {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    m_session.Selection().Canonicalize(m_session.DocumentView());
    auto composite = std::make_unique<CompositeEditCommand>("Duplicate Controls");
    std::vector<Uuid> roots;
    for (const Uuid& id : m_session.Selection().OrderedItems()) {
        if (id == m_session.DocumentView().Root()) continue;
        const auto* node = m_session.DocumentView().Find(*document, id);
        if (!node) continue;
        auto captured = document->CaptureSubtree(id);
        if (!captured) return Status::Fail(captured.Diagnostics());
        std::vector<Uuid> generated;
        RegenerateIds(captured.Value(), &generated);
        if (auto* name = captured.Value()["name"].TryGet<std::string>()) *name += " Copy";
        const Uuid root = *captured.Value()["id"].TryGet<Uuid>();
        roots.push_back(root);
        composite->Add(std::make_unique<SubtreeEditCommand>(
            "Duplicate " + node->name, SubtreeOperation::Insert, root, node->parent,
            document->ChildIndex(id) + 1, captured.TakeValue()));
    }
    if (composite->Empty()) return Status::Ok();
    const Status status = Execute(std::move(composite), DocumentChangeSet::Structure());
    if (status) {
        const Uuid primary = roots.empty() ? Uuid{} : roots.back();
        m_session.Selection().Replace(std::move(roots), primary);
    }
    return status;
}

Status DesignerCommandService::Reparent(const Uuid& target, const Uuid& newParent,
                                        std::size_t newIndex) {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    const auto* node = m_session.DocumentView().Find(*document, target);
    const auto* parent = m_session.DocumentView().Find(*document, newParent);
    if (!node || !parent) return Status::Fail(Error("PXEDCMD6004", "Reparent target is missing", target));
    if (parent->type == "ComponentInstance")
        return Status::Fail(Error("PXEDCMD6005", "Component instance structure is locked", newParent));
    return Execute(std::make_unique<ReparentEditCommand>(
                       "Reparent " + node->name, target, node->parent,
                       document->ChildIndex(target), newParent, newIndex),
                   DocumentChangeSet::Structure(newParent));
}

Status DesignerCommandService::Reorder(const Uuid& target, std::size_t newIndex) {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    const auto* node = m_session.DocumentView().Find(*document, target);
    if (!node || node->parent.Empty()) return Status::Ok();
    return Execute(std::make_unique<MoveChildEditCommand>(
                       "Reorder " + node->name, node->parent, target,
                       document->ChildIndex(target), newIndex),
                   DocumentChangeSet::Structure(node->parent));
}

Status DesignerCommandService::BeginPropertyGesture(const Uuid& target, std::string property,
                                                    std::string label,
                                                    DesignerDirtyFlags dirty) {
    CancelPropertyGesture();
    auto* document = m_session.Document();
    if (!document) return Status::Fail(Error("PXEDCMD6002", "No UI document is active"));
    m_gestureTargets = {target};
    m_gestureProperty = property;
    m_gestureHistoryCursor = document->History().Cursor();
    m_transactionChanges = DocumentChangeSet::Property(target, property, dirty);
    m_transaction = std::make_unique<PropertyEditTransaction>(
        *document, document->History(), target, std::move(property), std::move(label));
    return Status::Ok();
}

Status DesignerCommandService::BeginPropertyGesture(std::span<const Uuid> targets,
                                                    std::string property, std::string label,
                                                    DesignerDirtyFlags dirty) {
    CancelPropertyGesture();
    auto* document = m_session.Document();
    if (!document) return Status::Fail(Error("PXEDCMD6002", "No UI document is active"));
    std::vector<Uuid> copiedTargets(targets.begin(), targets.end());
    m_gestureTargets = copiedTargets;
    m_gestureProperty = property;
    m_gestureHistoryCursor = document->History().Cursor();
    m_transactionChanges = {};
    for (const Uuid& target : copiedTargets)
        m_transactionChanges.Merge(DocumentChangeSet::Property(target, property, dirty));
    m_multiTransaction = std::make_unique<MultiPropertyEditTransaction>(
        *document, document->History(), std::move(copiedTargets), std::move(property),
        std::move(label));
    return Status::Ok();
}

Status DesignerCommandService::UpdatePropertyGesture(Variant value) {
    if (!m_transaction && !m_multiTransaction)
        return Status::Fail(Error("PXEDCMD6006", "No property gesture is active"));
    const Status status = m_transaction ? m_transaction->Update(std::move(value))
                                        : m_multiTransaction->Update(value);
    if (status) {
        m_session.MarkDirty(m_transactionChanges.dirty);
        if (m_changed) m_changed(m_transactionChanges);
    }
    return status;
}

Status DesignerCommandService::CommitPropertyGesture() {
    if (!m_transaction && !m_multiTransaction) return Status::Ok();
    const Status status = m_transaction ? m_transaction->Commit() : m_multiTransaction->Commit();
    m_transaction.reset();
    m_multiTransaction.reset();
    const auto changes = std::move(m_transactionChanges);
    m_transactionChanges = {};
    m_gestureTargets.clear();
    m_gestureProperty.clear();
    if (!status) return status;
    if (auto* document = m_session.Document())
        RecordHistoryChange(m_gestureHistoryCursor, document->History().Cursor(), changes);
    return After(changes);
}

Status DesignerCommandService::CancelPropertyGesture() {
    if (!m_transaction && !m_multiTransaction) return Status::Ok();
    const Status status = m_transaction ? m_transaction->Cancel() : m_multiTransaction->Cancel();
    m_transaction.reset();
    m_multiTransaction.reset();
    const auto changes = std::move(m_transactionChanges);
    m_transactionChanges = {};
    m_gestureTargets.clear();
    m_gestureProperty.clear();
    if (status && m_changed) m_changed(changes);
    return status;
}

bool DesignerCommandService::GestureMatches(std::span<const Uuid> targets,
                                            std::string_view property) const {
    return GestureActive() && property == m_gestureProperty &&
           std::equal(targets.begin(), targets.end(), m_gestureTargets.begin(),
                      m_gestureTargets.end());
}

}  // namespace px::editor
