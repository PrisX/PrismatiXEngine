#include "Editor/Workspace/UndoStack.h"

namespace px::editor {

void UndoStack::Record(Command cmd) {
    m_undo.push_back(std::move(cmd));
    m_redo.clear();
}

bool UndoStack::Undo() {
    if (m_undo.empty()) return false;
    Command c = std::move(m_undo.back());
    m_undo.pop_back();
    if (c.undo) c.undo();
    m_redo.push_back(std::move(c));
    return true;
}

bool UndoStack::Redo() {
    if (m_redo.empty()) return false;
    Command c = std::move(m_redo.back());
    m_redo.pop_back();
    if (c.redo) c.redo();
    m_undo.push_back(std::move(c));
    return true;
}

void UndoStack::Clear() {
    m_undo.clear();
    m_redo.clear();
}

}
