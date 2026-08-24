#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"

#include <cassert>
#include <string>
#include <utility>

namespace {

px::vn::Command Say(std::string sourceId, std::string text) {
    px::vn::Command command;
    command.type = "say";
    command.sourceId = std::move(sourceId);
    command.args.push_back({"speaker", "雪"});
    command.args.push_back({"text", std::move(text)});
    return command;
}

px::vn::Program DialogueProgram(std::string text,
                                const bool appendOperation = false) {
    px::vn::Program program;
    program.code.push_back(Say("block-01", std::move(text)));
    program.branch.push_back(-1);
    if (appendOperation) {
        program.code.push_back(Say("block-02", "新增段落"));
        program.branch.push_back(-1);
    }
    return program;
}

}  // namespace

int main() {
    px::io::VFS vfs;
    px::audio::AudioEngine audio(vfs);
    px::graphics::AssetCache assets(nullptr, vfs);
    px::graphics::Renderer2D renderer(nullptr, assets);
    px::vn::Stage stage(renderer, assets);
    px::vn::Dialogue dialogue;
    px::vn::VariableStore variables;
    px::vn::Backlog backlog;
    px::vn::VM vm(vfs, audio, stage, dialogue, variables, backlog);

    constexpr const char* path = "memory://preview/runtime-ir.json";
    assert(vm.LoadCompiledProgram(DialogueProgram("修改前"), path));
    assert(vm.State() == px::vn::VMState::WaitingClick);
    assert(vm.ProgramCounter() == 1);
    assert(vm.CurrentSourceId() == "block-01");
    assert(dialogue.State().fullText == "修改前");
    assert(backlog.Entries().size() == 1);

    const auto patched =
        vm.PatchCompiledProgram(DialogueProgram("修改後"), path);
    assert(patched == px::vn::ProgramPatchStatus::Applied);
    assert(vm.State() == px::vn::VMState::WaitingClick);
    assert(vm.ProgramCounter() == 1);
    assert(vm.CurrentSourceId() == "block-01");
    assert(dialogue.State().fullText == "修改後");
    assert(backlog.Entries().size() == 1);

    const auto structural = vm.PatchCompiledProgram(
        DialogueProgram("不應套用", true), path);
    assert(structural == px::vn::ProgramPatchStatus::StructuralChange);
    assert(dialogue.State().fullText == "修改後");
    assert(vm.CurrentProgram().code.size() == 1);

    backlog.Clear();
    const auto seek = vm.LoadCompiledProgramAt(
        DialogueProgram("第一段", true), path, 1);
    assert(seek == px::vn::ProgramSeekStatus::Applied);
    assert(vm.State() == px::vn::VMState::WaitingClick);
    assert(vm.ProgramCounter() == 2);
    assert(vm.CurrentSourceId() == "block-02");
    assert(dialogue.State().fullText == "新增段落");
    assert(backlog.Entries().empty());

    const auto invalidSeek = vm.LoadCompiledProgramAt(
        DialogueProgram("第一段", true), path, 2);
    assert(invalidSeek == px::vn::ProgramSeekStatus::InvalidOperation);

    return 0;
}
