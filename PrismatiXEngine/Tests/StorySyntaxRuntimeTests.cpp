// Runtime support for Story syntax added in the 2026-09-30 release review:
// conditional choices and [elseif] lowering.
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Session/RuntimeIrAdapter.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"

#include <cassert>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

px::sdk::RuntimeIrOperation Operation(
    std::string id, std::string kind,
    std::unordered_map<std::string, std::string> arguments = {}) {
    return {id, id, 1, std::move(kind), {}, std::move(arguments)};
}

std::string Greater(const std::string& name, const int value) {
    return R"({"kind":"binary","op":"greater","left":{"kind":"variable","name":")" + name +
           R"("},"right":{"kind":"literal","value":)" + std::to_string(value) + "}}";
}

px::vn::Program Compile(std::vector<px::sdk::RuntimeIrOperation> operations) {
    px::sdk::RuntimeIrDocument document;
    document.documentId = "story-syntax";
    document.committedRevision = 1;
    document.operations = std::move(operations);
    px::vn::Program program = px::CompileRuntimeIr(document);
    for (const auto& error : program.errors) std::cerr << error << '\n';
    assert(program.errors.empty());
    return program;
}

px::sdk::RuntimeIrOperation SetAffection(const int value) {
    return Operation("set", "setVariable", {{"name", "affection"},
        {"value", std::to_string(value)}, {"scope", "session"}, {"type", "integer"}});
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
    constexpr const char* path = "memory://story-syntax.pxir";

    // A conditional option is hidden while false; the rest keep their order.
    const auto choices = [](const int affection) {
        return Compile({
            SetAffection(affection),
            Operation("confess", "choiceOption", {{"text", "告白する"}, {"target", "confess"},
                {"condition", Greater("affection", 3)}}),
            Operation("walk", "choiceOption", {{"text", "散歩する"}, {"target", "walk"}}),
            Operation("confess-label", "label", {{"target", "confess"}}),
            Operation("confess-line", "narration", {{"text", "好きです。"}}),
            Operation("end", "endStory"),
            Operation("walk-label", "label", {{"target", "walk"}}),
            Operation("walk-line", "narration", {{"text", "歩こう。"}}),
        });
    };
    assert(vm.LoadCompiledProgram(choices(1), path));
    assert(vm.State() == px::vn::VMState::WaitingChoice);
    assert(vm.Choices().size() == 1 && vm.Choices()[0].text == "散歩する");
    vm.SelectChoice(0);
    assert(dialogue.State().fullText == "歩こう。");

    assert(vm.LoadCompiledProgram(choices(5), path));
    assert(vm.Choices().size() == 2 && vm.Choices()[0].text == "告白する");
    vm.SelectChoice(0);
    assert(dialogue.State().fullText == "好きです。");

    // Every option hidden: the block is skipped instead of waiting forever.
    assert(vm.LoadCompiledProgram(Compile({
        SetAffection(0),
        Operation("only", "choiceOption", {{"text", "告白する"}, {"target", "after"},
            {"condition", Greater("affection", 3)}}),
        Operation("after-label", "label", {{"target", "after"}}),
        Operation("after-line", "narration", {{"text", "何も言えなかった。"}}),
    }), path));
    assert(vm.State() == px::vn::VMState::WaitingClick);
    assert(dialogue.State().fullText == "何も言えなかった。");

    // A hidden first option does not split the block at the next label.
    auto labelled = Compile({
        SetAffection(0),
        Operation("hidden", "choiceOption", {{"text", "A"}, {"target", "a"},
            {"condition", Greater("affection", 3)}}),
        Operation("visible", "choiceOption", {{"text", "B"}, {"target", "b"}}),
        Operation("a", "label", {{"target", "a"}}),
        Operation("b", "label", {{"target", "b"}}),
        Operation("line", "narration", {{"text", "B"}}),
    });
    assert(vm.LoadCompiledProgram(std::move(labelled), path));
    assert(vm.State() == px::vn::VMState::WaitingChoice && vm.Choices().size() == 1);

    // An invalid condition fails at load like an invalid [if].
    px::sdk::RuntimeIrDocument invalid;
    invalid.documentId = "invalid";
    invalid.operations = {Operation("bad", "choiceOption",
        {{"text", "A"}, {"target", "a"}, {"condition", "{not json"}})};
    assert(!px::CompileRuntimeIr(invalid).errors.empty());

    // [elseif] lowers to else + a nested condition closed by the one [endif].
    const auto chain = [](const int affection) {
        return Compile({
            SetAffection(affection),
            Operation("if", "condition", {{"expression", Greater("affection", 5)}}),
            Operation("high", "narration", {{"text", "high"}}),
            Operation("elseif", "else"),
            Operation("elseif/elseif", "condition", {{"expression", Greater("affection", 2)}}),
            Operation("middle", "narration", {{"text", "middle"}}),
            Operation("else", "else"),
            Operation("low", "narration", {{"text", "low"}}),
            Operation("endif", "endCondition"),
            Operation("endif/elseif1", "endCondition"),
            Operation("after", "narration", {{"text", "after"}}),
        });
    };
    for (const auto& [affection, expected] : std::vector<std::pair<int, std::string>>{
             {9, "high"}, {3, "middle"}, {0, "low"}}) {
        assert(vm.LoadCompiledProgram(chain(affection), path));
        assert(dialogue.State().fullText == expected);
        dialogue.ShowAll();
        vm.OnAdvance();
        assert(dialogue.State().fullText == "after");
    }

    // Reveal tags: [w] pauses, [speed N%] rescales and [nowait] is instant.
    // The tags never reach the visible text or the backlog.
    {
        px::vn::Dialogue reveal;
        reveal.SetText("", "あ[w 500ms]い[speed 50%]う[/speed][nowait]えお[/nowait]か[w 1s]",
                       30, {}, {});
        assert(reveal.State().fullText == "あいうえおか");
        assert(reveal.State().totalChars == 6);
        const auto at = [&reveal](const std::uint64_t now) {
            reveal.Update(now);
            return reveal.State().displayText;
        };
        assert(at(1000).empty());
        assert(at(1029).empty());
        assert(at(1030) == "あ");
        assert(at(1559) == "あ");
        assert(at(1560) == "あい");
        assert(at(1619) == "あい");
        assert(at(1620) == "あいうえお");
        assert(at(1650) == "あいうえおか" && reveal.Finished());

        px::vn::Dialogue fast;
        fast.SetText("", "[speed 200%]ab[/speed]", 30, {}, {});
        fast.Update(5000);
        fast.Update(5014);
        assert(fast.State().displayText.empty());
        fast.Update(5015);
        assert(fast.State().displayText == "a");
        // Malformed tags stay text for the markup renderer, as before.
        px::vn::Dialogue malformed;
        malformed.SetText("", "[w soon]x[speed 5%]y", 0, {}, {});
        assert(malformed.State().fullText == "[w soon]x[speed 5%]y");

        assert(vm.LoadCompiledProgram(Compile({
            Operation("tagged", "narration", {{"text", "待って[w 300ms]ね[nowait]！[/nowait]"}}),
        }), path));
        assert(dialogue.State().fullText == "待ってね！");
        assert(!backlog.Entries().empty() && backlog.Entries().back().text == "待ってね！");
    }

    std::cout << "Story syntax runtime tests passed\n";
    return 0;
}
