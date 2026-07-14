#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Uuid.h"
#include "Engine/Core/Variant.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace px::editor {

class IEditableDocument {
public:
    virtual ~IEditableDocument() = default;
    [[nodiscard]] virtual Uuid DocumentId() const = 0;
    [[nodiscard]] virtual Result<Variant> ReadProperty(const Uuid& target,
                                                       const std::string& property) const = 0;
    virtual Status WriteProperty(const Uuid& target, const std::string& property,
                                 const Variant& value) = 0;
    [[nodiscard]] virtual Result<VariantObject> CaptureSubtree(const Uuid& target) const = 0;
    virtual Status InsertSubtree(const Uuid& parent, std::size_t index,
                                 const VariantObject& subtree) = 0;
    [[nodiscard]] virtual Result<VariantObject> RemoveSubtree(const Uuid& target) = 0;
    virtual Status Reparent(const Uuid& target, const Uuid& parent, std::size_t index) = 0;
    virtual Status MoveChild(const Uuid& parent, const Uuid& target, std::size_t index) = 0;
};

class EditCommand {
public:
    explicit EditCommand(std::string label) : m_label(std::move(label)) {}
    virtual ~EditCommand() = default;
    [[nodiscard]] const std::string& Label() const { return m_label; }
    virtual Status Apply(IEditableDocument& document) = 0;
    virtual Status Revert(IEditableDocument& document) = 0;
    virtual bool TryMerge(const EditCommand&) { return false; }
    [[nodiscard]] virtual std::size_t MemoryCost() const = 0;

private:
    std::string m_label;
};

class PropertyChangeCommand final : public EditCommand {
public:
    PropertyChangeCommand(std::string label, Uuid target, std::string property, Variant before,
                          Variant after,
                          std::chrono::steady_clock::time_point committedAt =
                              std::chrono::steady_clock::now(), bool mergeable = true);
    Status Apply(IEditableDocument& document) override;
    Status Revert(IEditableDocument& document) override;
    bool TryMerge(const EditCommand& newer) override;
    [[nodiscard]] std::size_t MemoryCost() const override;

private:
    Uuid m_target;
    std::string m_property;
    Variant m_before;
    Variant m_after;
    std::chrono::steady_clock::time_point m_committedAt;
    bool m_mergeable = true;
};

class CompositeEditCommand final : public EditCommand {
public:
    explicit CompositeEditCommand(std::string label) : EditCommand(std::move(label)) {}
    void Add(std::unique_ptr<EditCommand> command);
    [[nodiscard]] bool Empty() const { return m_commands.empty(); }
    Status Apply(IEditableDocument& document) override;
    Status Revert(IEditableDocument& document) override;
    [[nodiscard]] std::size_t MemoryCost() const override;

private:
    std::vector<std::unique_ptr<EditCommand>> m_commands;
};

enum class SubtreeOperation { Insert, Remove };

class SubtreeEditCommand final : public EditCommand {
public:
    SubtreeEditCommand(std::string label, SubtreeOperation operation, Uuid target, Uuid parent,
                       std::size_t index, VariantObject subtree);
    Status Apply(IEditableDocument& document) override;
    Status Revert(IEditableDocument& document) override;
    [[nodiscard]] std::size_t MemoryCost() const override;

private:
    SubtreeOperation m_operation;
    Uuid m_target;
    Uuid m_parent;
    std::size_t m_index;
    VariantObject m_subtree;
};

class ReparentEditCommand final : public EditCommand {
public:
    ReparentEditCommand(std::string label, Uuid target, Uuid oldParent, std::size_t oldIndex,
                        Uuid newParent, std::size_t newIndex);
    Status Apply(IEditableDocument& document) override;
    Status Revert(IEditableDocument& document) override;
    [[nodiscard]] std::size_t MemoryCost() const override { return sizeof(*this); }

private:
    Uuid m_target;
    Uuid m_oldParent;
    std::size_t m_oldIndex;
    Uuid m_newParent;
    std::size_t m_newIndex;
};

class MoveChildEditCommand final : public EditCommand {
public:
    MoveChildEditCommand(std::string label, Uuid parent, Uuid target,
                         std::size_t oldIndex, std::size_t newIndex);
    Status Apply(IEditableDocument& document) override;
    Status Revert(IEditableDocument& document) override;
    [[nodiscard]] std::size_t MemoryCost() const override { return sizeof(*this); }

private:
    Uuid m_parent;
    Uuid m_target;
    std::size_t m_oldIndex;
    std::size_t m_newIndex;
};

class EditHistory {
public:
    explicit EditHistory(IEditableDocument& document) : m_document(document) {}
    Status Execute(std::unique_ptr<EditCommand> command);
    Status CommitApplied(std::unique_ptr<EditCommand> command);
    Status Undo();
    Status Redo();
    void MarkSaved();
    void Clear();
    [[nodiscard]] bool CanUndo() const { return m_cursor > 0; }
    [[nodiscard]] bool CanRedo() const { return m_cursor < m_commands.size(); }
    [[nodiscard]] bool Dirty() const;
    [[nodiscard]] std::string NextUndoLabel() const;
    [[nodiscard]] std::string NextRedoLabel() const;
    [[nodiscard]] std::size_t Cursor() const { return m_cursor; }

private:
    Status Record(std::unique_ptr<EditCommand> command);
    void EnforceLimits();
    void ReportFailure(const Status& status, const char* operation) const;
    IEditableDocument& m_document;
    std::vector<std::unique_ptr<EditCommand>> m_commands;
    std::size_t m_cursor = 0;
    std::optional<std::size_t> m_savedCursor = 0;
    std::size_t m_memory = 0;
    static constexpr std::size_t kMaxCommands = 500;
    static constexpr std::size_t kMaxMemory = 256u * 1024u * 1024u;
};

class PropertyEditTransaction {
public:
    PropertyEditTransaction(IEditableDocument& document, EditHistory& history, Uuid target,
                            std::string property, std::string label);
    ~PropertyEditTransaction();
    Status Update(Variant value);
    Status Commit();
    Status Cancel();
    [[nodiscard]] bool Active() const { return m_active; }

private:
    IEditableDocument& m_document;
    EditHistory& m_history;
    Uuid m_target;
    std::string m_property;
    std::string m_label;
    Variant m_before;
    Variant m_current;
    bool m_active = false;
};

class MultiPropertyEditTransaction {
public:
    MultiPropertyEditTransaction(IEditableDocument& document, EditHistory& history,
                                 std::vector<Uuid> targets, std::string property,
                                 std::string label);
    ~MultiPropertyEditTransaction();
    Status Update(const Variant& value);
    Status Commit();
    Status Cancel();
    [[nodiscard]] bool Active() const { return m_active; }

private:
    struct Entry { Uuid target; Variant before; Variant current; };
    IEditableDocument& m_document;
    EditHistory& m_history;
    std::vector<Entry> m_entries;
    std::string m_property;
    std::string m_label;
    bool m_active = false;
};

}  // namespace px::editor
