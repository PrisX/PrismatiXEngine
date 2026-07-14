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

Status DesignerCommandService::After(DesignerDirtyFlags dirty, bool structural) {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    if (structural) {
        const Status rebuilt = m_session.DocumentView().Rebuild(*document);
        if (!rebuilt) return rebuilt;
        m_session.Selection().Canonicalize(m_session.DocumentView());
        dirty |= DesignerDirtyFlags::Structure | DesignerDirtyFlags::Layout;
    }
    m_session.MarkDirty(dirty);
    if (m_changed) m_changed(dirty);
    return Status::Ok();
}

Status DesignerCommandService::Execute(std::unique_ptr<EditCommand> command,
                                       DesignerDirtyFlags dirty, bool structural) {
    auto* document = m_session.Document();
    if (!document || !command)
        return Status::Fail(Error("PXEDCMD6001", "No UI document or command is active"));
    const Status status = document->History().Execute(std::move(command));
    return status ? After(dirty, structural) : status;
}

Status DesignerCommandService::CommitApplied(std::unique_ptr<EditCommand> command,
                                             DesignerDirtyFlags dirty, bool structural) {
    auto* document = m_session.Document();
    if (!document || !command)
        return Status::Fail(Error("PXEDCMD6001", "No UI document or command is active"));
    const Status status = document->History().CommitApplied(std::move(command));
    return status ? After(dirty, structural) : status;
}

Status DesignerCommandService::Undo() {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    CancelPropertyGesture();
    const Status status = document->History().Undo();
    return status ? After(kAllDesignerContentDirty, true) : status;
}

Status DesignerCommandService::Redo() {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    CancelPropertyGesture();
    const Status status = document->History().Redo();
    return status ? After(kAllDesignerContentDirty, true) : status;
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
    return Execute(std::make_unique<PropertyChangeCommand>(
                       std::move(label), target, std::move(property), before.TakeValue(),
                       std::move(value)),
                   dirty, false);
}

Status DesignerCommandService::SetProperties(std::span<const Uuid> targets,
                                             const std::string& property,
                                             const Variant& value, std::string label,
                                             DesignerDirtyFlags dirty) {
    auto* document = m_session.Document();
    if (!document) return Status::Fail(Error("PXEDCMD6002", "No UI document is active"));
    auto composite = std::make_unique<CompositeEditCommand>(std::move(label));
    for (const Uuid& target : targets) {
        auto before = document->ReadProperty(target, property);
        if (!before || before.Value() == value) continue;
        composite->Add(std::make_unique<PropertyChangeCommand>(
            "Change " + property, target, property, before.TakeValue(), value.Clone()));
    }
    if (composite->Empty()) return Status::Ok();
    return Execute(std::move(composite), dirty, false);
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
    const Status status = Execute(std::move(composite), DesignerDirtyFlags::Structure, true);
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
    const Status status = Execute(std::move(composite), DesignerDirtyFlags::Structure, true);
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
                   DesignerDirtyFlags::Structure, true);
}

Status DesignerCommandService::Reorder(const Uuid& target, std::size_t newIndex) {
    auto* document = m_session.Document();
    if (!document) return Status::Ok();
    const auto* node = m_session.DocumentView().Find(*document, target);
    if (!node || node->parent.Empty()) return Status::Ok();
    return Execute(std::make_unique<MoveChildEditCommand>(
                       "Reorder " + node->name, node->parent, target,
                       document->ChildIndex(target), newIndex),
                   DesignerDirtyFlags::Structure, true);
}

Status DesignerCommandService::BeginPropertyGesture(const Uuid& target, std::string property,
                                                    std::string label,
                                                    DesignerDirtyFlags dirty) {
    CancelPropertyGesture();
    auto* document = m_session.Document();
    if (!document) return Status::Fail(Error("PXEDCMD6002", "No UI document is active"));
    m_transaction = std::make_unique<PropertyEditTransaction>(
        *document, document->History(), target, std::move(property), std::move(label));
    m_transactionDirty = dirty;
    return Status::Ok();
}

Status DesignerCommandService::UpdatePropertyGesture(Variant value) {
    if (!m_transaction) return Status::Fail(Error("PXEDCMD6006", "No property gesture is active"));
    const Status status = m_transaction->Update(std::move(value));
    if (status) {
        m_session.MarkDirty(m_transactionDirty);
        if (m_changed) m_changed(m_transactionDirty);
    }
    return status;
}

Status DesignerCommandService::CommitPropertyGesture() {
    if (!m_transaction) return Status::Ok();
    const Status status = m_transaction->Commit();
    m_transaction.reset();
    return status ? After(m_transactionDirty, false) : status;
}

Status DesignerCommandService::CancelPropertyGesture() {
    if (!m_transaction) return Status::Ok();
    const Status status = m_transaction->Cancel();
    m_transaction.reset();
    if (status && m_changed) m_changed(m_transactionDirty);
    return status;
}

}  // namespace px::editor
