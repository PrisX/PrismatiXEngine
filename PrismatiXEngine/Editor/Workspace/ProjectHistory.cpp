#include "Editor/Workspace/ProjectHistory.h"

#include "Engine/Diagnostics/Diagnostic.h"

namespace px::editor {
namespace {
Status InvalidProjectCommand(const char* operation) {
    return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
        .code="PXPROJECTEDIT9101",.category="Editor.ProjectHistory",
        .message=std::string(operation)+" 缺少有效的專案命令"});
}
}

Status ProjectCommandHistory::Execute(std::unique_ptr<ProjectCommand> command) {
    if (!command) return InvalidProjectCommand("Execute");
    const Status status=command->Apply();if(!status){Report(status,"apply");return status;}
    return Record(std::move(command));
}
Status ProjectCommandHistory::CommitApplied(std::unique_ptr<ProjectCommand> command) {
    return command?Record(std::move(command)):InvalidProjectCommand("CommitApplied");
}
Status ProjectCommandHistory::Undo(){if(!CanUndo())return Status::Ok();const Status status=m_commands[m_cursor-1]->Revert();if(!status){Report(status,"undo");return status;}--m_cursor;return Status::Ok();}
Status ProjectCommandHistory::Redo(){if(!CanRedo())return Status::Ok();const Status status=m_commands[m_cursor]->Apply();if(!status){Report(status,"redo");return status;}++m_cursor;return Status::Ok();}
void ProjectCommandHistory::Clear(){m_commands.clear();m_cursor=0;}
std::string ProjectCommandHistory::NextUndoLabel()const{return CanUndo()?m_commands[m_cursor-1]->Label():std::string{};}
std::string ProjectCommandHistory::NextRedoLabel()const{return CanRedo()?m_commands[m_cursor]->Label():std::string{};}
Status ProjectCommandHistory::Record(std::unique_ptr<ProjectCommand> command){if(m_cursor<m_commands.size())m_commands.erase(m_commands.begin()+static_cast<std::ptrdiff_t>(m_cursor),m_commands.end());m_commands.push_back(std::move(command));++m_cursor;if(m_commands.size()>kMaxCommands){m_commands.erase(m_commands.begin());--m_cursor;}return Status::Ok();}
void ProjectCommandHistory::Report(const Status& status,const char* operation)const{for(auto diagnostic:status.Diagnostics()){diagnostic.category="Editor.ProjectHistory";diagnostic.operationId=operation;diag::Emit(std::move(diagnostic));}}

}  // namespace px::editor
