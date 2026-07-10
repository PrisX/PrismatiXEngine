#pragma once

#include "Engine/Core/Result.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace px::editor {

class ProjectCommand {
public:
    explicit ProjectCommand(std::string label) : m_label(std::move(label)) {}
    virtual ~ProjectCommand() = default;
    [[nodiscard]] const std::string& Label() const { return m_label; }
    virtual Status Apply() = 0;
    virtual Status Revert() = 0;
private:
    std::string m_label;
};

class FunctionalProjectCommand final : public ProjectCommand {
public:
    using Action = std::function<Status()>;
    FunctionalProjectCommand(std::string label, Action apply, Action revert)
        : ProjectCommand(std::move(label)), m_apply(std::move(apply)),
          m_revert(std::move(revert)) {}
    Status Apply() override { return m_apply ? m_apply() : Status::Ok(); }
    Status Revert() override { return m_revert ? m_revert() : Status::Ok(); }
private:
    Action m_apply;
    Action m_revert;
};

class ProjectCommandHistory {
public:
    Status Execute(std::unique_ptr<ProjectCommand> command);
    Status CommitApplied(std::unique_ptr<ProjectCommand> command);
    Status Undo();
    Status Redo();
    void Clear();
    [[nodiscard]] bool CanUndo() const { return m_cursor > 0; }
    [[nodiscard]] bool CanRedo() const { return m_cursor < m_commands.size(); }
    [[nodiscard]] std::string NextUndoLabel() const;
    [[nodiscard]] std::string NextRedoLabel() const;
private:
    Status Record(std::unique_ptr<ProjectCommand> command);
    void Report(const Status& status, const char* operation) const;
    std::vector<std::unique_ptr<ProjectCommand>> m_commands;
    std::size_t m_cursor = 0;
    static constexpr std::size_t kMaxCommands = 100;
};

}  // namespace px::editor
