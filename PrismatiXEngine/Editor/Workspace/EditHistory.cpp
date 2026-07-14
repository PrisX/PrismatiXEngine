#include "Editor/Workspace/EditHistory.h"

#include "Engine/Diagnostics/Diagnostic.h"

namespace px::editor {

namespace {
std::size_t VariantCost(const Variant& value) {
    std::size_t cost = sizeof(value);
    if (const auto* text = value.TryGet<std::string>()) cost += text->capacity();
    if (const auto* array = value.AsArray())
        for (const Variant& item : *array) cost += VariantCost(item);
    if (const auto* object = value.AsObject())
        for (const auto& [key, item] : *object) cost += key.capacity() + VariantCost(item);
    return cost;
}
Status InvalidCommand(const char* operation) {
    return Status::Fail(diag::Diagnostic{ diag::Severity::Error, "PXEDIT-E1001", "undo",
                                          std::string(operation) + " has no valid command." });
}
}

PropertyChangeCommand::PropertyChangeCommand(
    std::string label, Uuid target, std::string property, Variant before, Variant after,
    std::chrono::steady_clock::time_point committedAt,bool mergeable)
    : EditCommand(std::move(label)), m_target(target), m_property(std::move(property)),
      m_before(before.Clone()), m_after(after.Clone()), m_committedAt(committedAt),m_mergeable(mergeable) {}
Status PropertyChangeCommand::Apply(IEditableDocument& document) {
    return document.WriteProperty(m_target, m_property, m_after);
}
Status PropertyChangeCommand::Revert(IEditableDocument& document) {
    return document.WriteProperty(m_target, m_property, m_before);
}
bool PropertyChangeCommand::TryMerge(const EditCommand& newer) {
    const auto* property = dynamic_cast<const PropertyChangeCommand*>(&newer);
    if (!m_mergeable || !property || !property->m_mergeable || property->m_target != m_target || property->m_property != m_property ||
        property->m_committedAt - m_committedAt > std::chrono::milliseconds(750)) return false;
    m_after = property->m_after.Clone();
    m_committedAt = property->m_committedAt;
    return true;
}
std::size_t PropertyChangeCommand::MemoryCost() const {
    return sizeof(*this) + m_property.capacity() + VariantCost(m_before) + VariantCost(m_after);
}

void CompositeEditCommand::Add(std::unique_ptr<EditCommand> command) {
    if (command) m_commands.push_back(std::move(command));
}
Status CompositeEditCommand::Apply(IEditableDocument& document) {
    std::size_t applied = 0;
    for (; applied < m_commands.size(); ++applied) {
        Status status = m_commands[applied]->Apply(document);
        if (!status) {
            while (applied > 0) m_commands[--applied]->Revert(document);
            return status;
        }
    }
    return Status::Ok();
}
Status CompositeEditCommand::Revert(IEditableDocument& document) {
    for (std::size_t i = m_commands.size(); i > 0; --i) {
        Status status = m_commands[i - 1]->Revert(document);
        if (!status) return status;
    }
    return Status::Ok();
}
std::size_t CompositeEditCommand::MemoryCost() const {
    std::size_t cost = sizeof(*this);
    for (const auto& command : m_commands) cost += command->MemoryCost();
    return cost;
}

SubtreeEditCommand::SubtreeEditCommand(std::string label, SubtreeOperation operation, Uuid target,
                                       Uuid parent, std::size_t index, VariantObject subtree)
    : EditCommand(std::move(label)), m_operation(operation), m_target(target), m_parent(parent),
      m_index(index), m_subtree(std::move(subtree)) {}
Status SubtreeEditCommand::Apply(IEditableDocument& document) {
    if (m_operation == SubtreeOperation::Insert)
        return document.InsertSubtree(m_parent, m_index, m_subtree);
    auto removed = document.RemoveSubtree(m_target);
    return removed ? Status::Ok() : Status::Fail(removed.Diagnostics());
}
Status SubtreeEditCommand::Revert(IEditableDocument& document) {
    if (m_operation == SubtreeOperation::Remove)
        return document.InsertSubtree(m_parent, m_index, m_subtree);
    auto removed = document.RemoveSubtree(m_target);
    return removed ? Status::Ok() : Status::Fail(removed.Diagnostics());
}
std::size_t SubtreeEditCommand::MemoryCost() const {
    std::size_t cost = sizeof(*this);
    for (const auto& [key, value] : m_subtree) cost += key.capacity() + VariantCost(value);
    return cost;
}

ReparentEditCommand::ReparentEditCommand(std::string label, Uuid target, Uuid oldParent,
                                         std::size_t oldIndex, Uuid newParent,
                                         std::size_t newIndex)
    : EditCommand(std::move(label)), m_target(target), m_oldParent(oldParent),
      m_oldIndex(oldIndex), m_newParent(newParent), m_newIndex(newIndex) {}
Status ReparentEditCommand::Apply(IEditableDocument& document) {
    return document.Reparent(m_target, m_newParent, m_newIndex);
}
Status ReparentEditCommand::Revert(IEditableDocument& document) {
    return document.Reparent(m_target, m_oldParent, m_oldIndex);
}

MoveChildEditCommand::MoveChildEditCommand(std::string label, Uuid parent, Uuid target,
                                           std::size_t oldIndex, std::size_t newIndex)
    : EditCommand(std::move(label)), m_parent(parent), m_target(target),
      m_oldIndex(oldIndex), m_newIndex(newIndex) {}
Status MoveChildEditCommand::Apply(IEditableDocument& document) {
    return document.MoveChild(m_parent, m_target, m_newIndex);
}
Status MoveChildEditCommand::Revert(IEditableDocument& document) {
    return document.MoveChild(m_parent, m_target, m_oldIndex);
}

Status EditHistory::Execute(std::unique_ptr<EditCommand> command) {
    if (!command) return InvalidCommand("Execute");
    Status status = command->Apply(m_document);
    if (!status) { ReportFailure(status, "apply"); return status; }
    return Record(std::move(command));
}
Status EditHistory::CommitApplied(std::unique_ptr<EditCommand> command) {
    return command ? Record(std::move(command)) : InvalidCommand("Commit");
}
Status EditHistory::Undo() {
    if (!CanUndo()) return Status::Ok();
    Status status = m_commands[m_cursor - 1]->Revert(m_document);
    if (!status) { ReportFailure(status, "undo"); return status; }
    --m_cursor;
    return Status::Ok();
}
Status EditHistory::Redo() {
    if (!CanRedo()) return Status::Ok();
    Status status = m_commands[m_cursor]->Apply(m_document);
    if (!status) { ReportFailure(status, "redo"); return status; }
    ++m_cursor;
    return Status::Ok();
}
void EditHistory::MarkSaved() { m_savedCursor = m_cursor; }
void EditHistory::Clear() { m_commands.clear(); m_cursor = 0; m_savedCursor = 0; m_memory = 0; }
bool EditHistory::Dirty() const { return !m_savedCursor || *m_savedCursor != m_cursor; }
std::string EditHistory::NextUndoLabel() const { return CanUndo() ? m_commands[m_cursor - 1]->Label() : std::string{}; }
std::string EditHistory::NextRedoLabel() const { return CanRedo() ? m_commands[m_cursor]->Label() : std::string{}; }

Status EditHistory::Record(std::unique_ptr<EditCommand> command) {
    if (m_cursor < m_commands.size()) {
        if (m_savedCursor && *m_savedCursor > m_cursor) m_savedCursor.reset();
        for (std::size_t i = m_cursor; i < m_commands.size(); ++i) m_memory -= m_commands[i]->MemoryCost();
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_commands.end());
    }
    if (m_cursor > 0 && m_commands[m_cursor - 1]->TryMerge(*command)) {
        m_memory = 0;
        for (const auto& item : m_commands) m_memory += item->MemoryCost();
    } else {
        m_memory += command->MemoryCost();
        m_commands.push_back(std::move(command));
        ++m_cursor;
    }
    EnforceLimits();
    return Status::Ok();
}
void EditHistory::EnforceLimits() {
    while (!m_commands.empty() && (m_commands.size() > kMaxCommands || m_memory > kMaxMemory)) {
        m_memory -= m_commands.front()->MemoryCost();
        m_commands.erase(m_commands.begin());
        if (m_cursor > 0) --m_cursor;
        if (m_savedCursor) {
            if (*m_savedCursor == 0) m_savedCursor.reset();
            else --*m_savedCursor;
        }
    }
}
void EditHistory::ReportFailure(const Status& status, const char* operation) const {
    for (diag::Diagnostic diagnostic : status.Diagnostics()) {
        diagnostic.category = "undo";
        diagnostic.operationId = operation;
        diag::Emit(std::move(diagnostic));
    }
}

PropertyEditTransaction::PropertyEditTransaction(IEditableDocument& document, EditHistory& history,
                                                 Uuid target, std::string property,
                                                 std::string label)
    : m_document(document), m_history(history), m_target(target),
      m_property(std::move(property)), m_label(std::move(label)) {
    auto before = m_document.ReadProperty(m_target, m_property);
    if (before) { m_before = before.TakeValue(); m_current = m_before; m_active = true; }
    else for (auto d : before.Diagnostics()) diag::Emit(std::move(d));
}
PropertyEditTransaction::~PropertyEditTransaction() { if (m_active) Cancel(); }
Status PropertyEditTransaction::Update(Variant value) {
    if (!m_active) return InvalidCommand("Transaction update");
    Status status = m_document.WriteProperty(m_target, m_property, value);
    if (status) m_current = std::move(value);
    return status;
}
Status PropertyEditTransaction::Commit() {
    if (!m_active) return Status::Ok();
    m_active = false;
    if (m_before == m_current) return Status::Ok();
    return m_history.CommitApplied(std::make_unique<PropertyChangeCommand>(
        m_label, m_target, m_property, std::move(m_before), std::move(m_current)));
}
Status PropertyEditTransaction::Cancel() {
    if (!m_active) return Status::Ok();
    m_active = false;
    return m_document.WriteProperty(m_target, m_property, m_before);
}

MultiPropertyEditTransaction::MultiPropertyEditTransaction(
    IEditableDocument& document,EditHistory& history,std::vector<Uuid> targets,
    std::string property,std::string label)
    :m_document(document),m_history(history),m_property(std::move(property)),m_label(std::move(label)){
    for(const Uuid& target:targets){auto before=m_document.ReadProperty(target,m_property);if(!before){for(auto diagnostic:before.Diagnostics())diag::Emit(std::move(diagnostic));m_entries.clear();return;}m_entries.push_back({target,before.Value(),before.Value()});}
    m_active=!m_entries.empty();
}
MultiPropertyEditTransaction::~MultiPropertyEditTransaction(){if(m_active)(void)Cancel();}
Status MultiPropertyEditTransaction::Update(const Variant& value){
    if(!m_active)return InvalidCommand("Multi-property transaction update");
    std::size_t written=0;for(auto& entry:m_entries){const Status status=m_document.WriteProperty(entry.target,m_property,value);if(!status){for(std::size_t index=0;index<written;++index)(void)m_document.WriteProperty(m_entries[index].target,m_property,m_entries[index].current);return status;}entry.current=value.Clone();++written;}return Status::Ok();
}
Status MultiPropertyEditTransaction::Commit(){
    if(!m_active)return Status::Ok();m_active=false;auto command=std::make_unique<CompositeEditCommand>(m_label);for(auto& entry:m_entries)if(entry.before!=entry.current)command->Add(std::make_unique<PropertyChangeCommand>(m_label,entry.target,m_property,std::move(entry.before),std::move(entry.current),std::chrono::steady_clock::now(),false));if(command->Empty())return Status::Ok();return m_history.CommitApplied(std::move(command));
}
Status MultiPropertyEditTransaction::Cancel(){
    if(!m_active)return Status::Ok();m_active=false;Status result;for(const auto& entry:m_entries){const Status status=m_document.WriteProperty(entry.target,m_property,entry.before);for(const auto& diagnostic:status.Diagnostics())result.Add(diagnostic);}return result;
}

}  // namespace px::editor
