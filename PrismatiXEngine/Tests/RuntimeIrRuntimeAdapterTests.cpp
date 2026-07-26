#include "Engine/Session/RuntimeIrAdapter.h"

#include <cassert>
#include <iostream>

namespace {

px::sdk::RuntimeIrOperation Operation(
    std::string id, std::string kind,
    std::unordered_map<std::string, std::string> arguments = {}) {
    return {id, id, std::move(kind), {}, std::move(arguments)};
}

}  // namespace

int main() {
    px::sdk::RuntimeIrDocument document;
    document.documentId = "scene-document";
    document.committedRevision = 4;
    document.operations = {
        Operation("scene", "scene", {{"title", "雨夜"}}),
        Operation("set", "setVariable", {{"name", "affection"}, {"value", "3"}}),
        Operation("if", "condition", {{"expression", "affection >= 3"}}),
        Operation("say-true", "dialogue", {{"speaker", "雪"}, {"text", "你還記得。"}}),
        Operation("else", "else"),
        Operation("say-false", "dialogue", {{"speaker", "雪"}, {"text", "你忘了。"}}),
        Operation("end", "endCondition"),
        Operation("voice", "voice", {{"asset", "yuki/001.ogg"}}),
        Operation("wait", "wait", {{"duration", "0.4s"}}),
    };

    const px::vn::Program program = px::CompileRuntimeIr(document);
    for (const auto& error : program.errors) std::cerr << error << '\n';
    assert(program.errors.empty());
    assert(program.code.front().type == "chapter");
    assert(program.code[1].type == "var");
    assert(program.code[2].type == "branch");
    assert(program.code[2].FindTyped("expression") != nullptr);
    assert(program.LabelIndex("@if-else-if") >= 0);
    assert(program.LabelIndex("@if-end-if") >= 0);
    assert(program.code.back().type == "wait");
    assert(program.code.back().Get("ms") == "400");

    px::sdk::RuntimeIrDocument fragmentDocument;
    fragmentDocument.documentId = "fragment-scene";
    fragmentDocument.committedRevision = 2;
    fragmentDocument.operations = {
        Operation("root", "scene", {{"title", "Fragment Test"}}),
        Operation("call", "callFragment", {{"target", "greeting"}}),
        Operation("end-story", "endStory"),
        Operation("fragment", "fragment", {{"target", "greeting"}}),
        Operation("fragment-say", "narration", {{"text", "Hello"}}),
        Operation("fragment-return", "return"),
    };
    const px::vn::Program fragmentProgram = px::CompileRuntimeIr(fragmentDocument);
    for (const auto& error : fragmentProgram.errors) std::cerr << error << '\n';
    assert(fragmentProgram.errors.empty());
    assert(fragmentProgram.LabelIndex("@greeting") >= 0);
    assert(fragmentProgram.LabelIndex("@document-end") >= 0);
    bool foundCall = false;
    bool foundReturn = false;
    for (const auto& command : fragmentProgram.code) {
        foundCall = foundCall || command.type == "call";
        foundReturn = foundReturn || command.type == "return";
    }
    assert(foundCall);
    assert(foundReturn);
    return 0;
}
