#pragma once

#include <functional>
#include <string>
#include <vector>

namespace px::editor {

class UndoStack {
public:
    struct Command {
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    void Record(Command cmd);
    bool Undo();
    bool Redo();
    void Clear();
    [[nodiscard]] bool CanUndo() const { return !m_undo.empty(); }
    [[nodiscard]] bool CanRedo() const { return !m_redo.empty(); }
    [[nodiscard]] std::string NextUndoLabel() const {
        return m_undo.empty() ? "" : m_undo.back().label;
    }
    [[nodiscard]] std::string NextRedoLabel() const {
        return m_redo.empty() ? "" : m_redo.back().label;
    }

private:
    std::vector<Command> m_undo;
    std::vector<Command> m_redo;
};

}
