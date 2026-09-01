#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <utility>
#include <vector>

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

px::vn::Program InfiniteJumpProgram() {
    px::vn::Command jump;
    jump.type = "jump";
    jump.sourceId = "loop-jump";
    jump.operationId = "loop-operation";
    jump.line = 42;
    jump.args.push_back({"target", "loop"});
    px::vn::Program program;
    program.code.push_back(std::move(jump));
    program.labels["loop"] = 0;
    program.branch.push_back(0);
    return program;
}

px::vn::Program ChoiceProgram(const std::string& documentId) {
    px::vn::Command choice;
    choice.type = "choice";
    choice.sourceId = "shared-source";
    choice.operationId = "shared-operation";
    choice.args.push_back({"text", "Choose"});
    px::vn::Program program;
    program.documentId = documentId;
    program.code.push_back(std::move(choice));
    program.branch.push_back(-1);
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

    std::vector<std::string> choiceSeen;
    vm.SetChoiceSeenHook(
        [&choiceSeen](const std::string& key) { choiceSeen.push_back(key); });
    assert(vm.LoadCompiledProgram(ChoiceProgram("document-a"), path));
    vm.SelectChoice(0);
    assert(vm.LoadCompiledProgram(ChoiceProgram("document-b"), path));
    vm.SelectChoice(0);
    assert(choiceSeen.size() == 2 && choiceSeen[0] != choiceSeen[1]);
    assert(choiceSeen[0] ==
           "document-a:shared-source:shared-operation");

    px::vn::VMConfig limits;
    limits.maxInstructionsPerTick = 4;
    limits.maxInstructionsWithoutYield = 12;
    vm.SetConfig(limits);
    assert(vm.LoadCompiledProgram(InfiniteJumpProgram(), path));
    assert(vm.State() == px::vn::VMState::Running);
    vm.Update(1, 0.016f);
    vm.Update(2, 0.016f);
    assert(vm.State() == px::vn::VMState::Finished);
    assert(vm.LastRunResult().status == px::vn::VMRunStatus::Faulted);

    const auto invalidMap = px::sdk::ParseSourceMap(R"({
      "format":"PrismatiXSourceMap","schemaRevision":2,"documentId":"doc",
      "mappings":[{"operationId":"op","sourceId":"source",
        "sourceUri":"../outside.pxstory","startLine":1,"startColumn":1,
        "endLine":1,"endColumn":2}]
    })");
    assert(!invalidMap.Valid());

    px::RuntimeSession session({vfs, audio, renderer, assets});
    assert(session.SetSourceMapText(R"({
      "format":"PrismatiXSourceMap","schemaRevision":2,"documentId":"loop-document",
      "mappings":[
        {"operationId":"label-operation","sourceId":"label-source",
         "sourceUri":"Story/Loop.pxstory","startLine":6,"startColumn":1,
         "endLine":6,"endColumn":7},
        {"operationId":"jump-operation","sourceId":"jump-source",
         "sourceUri":"Story/Loop.pxstory","startLine":77,"startColumn":3,
         "endLine":77,"endColumn":14}
      ]
    })", "Content/Runtime/loop.pxmap"));
    px::vn::VMConfig mappedLimits;
    mappedLimits.maxInstructionsPerTick = 3;
    mappedLimits.maxInstructionsWithoutYield = 9;
    session.VM().SetConfig(mappedLimits);
    std::vector<px::diag::Diagnostic> emitted;
    px::diag::Global().SetListener(
        [&emitted](const px::diag::Diagnostic& diagnostic) {
            emitted.push_back(diagnostic);
        });
    assert(session.StartRuntimeIrText(R"({
      "format":"PrismatiXRuntimeIR","schemaRevision":2,
      "documentId":"loop-document","committedRevision":1,
      "operations":[
        {"operationId":"label-operation","sourceId":"label-source",
         "sourceLine":6,"kind":"label","text":"loop",
         "arguments":{"target":"loop"}},
        {"operationId":"jump-operation","sourceId":"jump-source",
         "sourceLine":77,"kind":"jump","text":"loop",
         "arguments":{"target":"loop"}}
      ]
    })", "Content/Runtime/loop.pxir"));
    session.Update(1, 0.016f);
    session.Update(2, 0.016f);
    px::diag::Global().SetListener({});
    const auto mappedFatal = std::find_if(
        emitted.begin(), emitted.end(), [](const auto& diagnostic) {
            return diagnostic.code == "PXRUNTIME7311";
        });
    assert(mappedFatal != emitted.end());
    assert(mappedFatal->source.path == "Story/Loop.pxstory");
    assert(mappedFatal->source.line == 77);
    assert(mappedFatal->source.column == 3);
    assert(mappedFatal->source.endLine == 77);
    assert(mappedFatal->source.endColumn == 14);
    assert(mappedFatal->documentId == "loop-document");
    assert(mappedFatal->sourceId == "jump-source");
    assert(mappedFatal->operationId == "jump-operation");

    return 0;
}
