#include "Engine/Session/RuntimeIrAdapter.h"
#include "Engine/VN/Commands/CommandRegistry.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <set>

#include <nlohmann/json.hpp>

namespace {

px::sdk::RuntimeIrOperation Operation(
    std::string id, std::string kind,
    std::unordered_map<std::string, std::string> arguments = {},
    const std::uint32_t sourceLine = 0) {
    return {id, id, sourceLine, std::move(kind), {}, std::move(arguments)};
}

}  // namespace

int main() {
    px::vn::CommandDescriptor typedDescriptor;
    typedDescriptor.id = "game.setMood";
    typedDescriptor.displayName = "Set Mood";
    typedDescriptor.allowAdditionalParameters = false;
    px::vn::CommandParameterDescriptor mode;
    mode.name = "mood";
    mode.label = "Mood";
    mode.type = px::VariantType::String;
    mode.defaultValue = px::Variant("calm");
    mode.hasDefault = true;
    mode.required = true;
    mode.options = {"calm", "tense"};
    typedDescriptor.parameters.push_back(std::move(mode));
    px::vn::CommandParameterDescriptor amount;
    amount.name = "strength";
    amount.label = "Strength";
    amount.type = px::VariantType::Number;
    amount.minimum = 0.0;
    amount.maximum = 1.0;
    typedDescriptor.parameters.push_back(std::move(amount));
    px::vn::CommandParameterDescriptor enabled;
    enabled.name = "enabled";
    enabled.label = "Enabled";
    enabled.type = px::VariantType::Bool;
    enabled.defaultValue = px::Variant(true);
    enabled.hasDefault = true;
    typedDescriptor.parameters.push_back(std::move(enabled));
    px::vn::CommandParameterDescriptor asset;
    asset.name = "asset";
    asset.label = "Asset";
    asset.type = px::VariantType::ResourceRef;
    typedDescriptor.parameters.push_back(std::move(asset));
    const auto addParameter = [&typedDescriptor](std::string name,
                                                  const px::VariantType type) {
        px::vn::CommandParameterDescriptor parameter;
        parameter.name = std::move(name);
        parameter.label = parameter.name;
        parameter.type = type;
        typedDescriptor.parameters.push_back(std::move(parameter));
    };
    addParameter("target", px::VariantType::Uuid);
    addParameter("offset", px::VariantType::Vec2);
    addParameter("bounds", px::VariantType::Rect);
    addParameter("tint", px::VariantType::Color);
    addParameter("tags", px::VariantType::Array);
    addParameter("metadata", px::VariantType::Object);
    const auto registration =
        px::vn::CommandRegistry::Global().Register(std::move(typedDescriptor));
    assert(registration.IsOk());

    px::sdk::RuntimeIrDocument document;
    document.documentId = "scene-document";
    document.committedRevision = 4;
    document.operations = {
        Operation("scene", "scene", {{"title", "雨夜"}}),
        Operation("set", "setVariable", {{"name", "affection"}, {"value", "3"}}),
        Operation("if", "condition", {{"expression", "affection >= 3"}}),
        Operation("say-true", "dialogue",
                  {{"speaker", "雪"}, {"text", "你還記得。"}}, 47),
        Operation("else", "else"),
        Operation("say-false", "dialogue", {{"speaker", "雪"}, {"text", "你忘了。"}}),
        Operation("end", "endCondition"),
        Operation("voice", "voice", {{"asset", "yuki/001.ogg"}}),
        Operation("typed-action", "customNode",
                  {{"type", "action"},
                   {"value", R"({"id":"demo.typed","arguments":{"amount":2,"asset":"asset:33333333-3333-4333-8333-333333333333"}})"}}),
        Operation("wait", "wait", {{"duration", "0.4s"}}),
    };

    const px::vn::Program program = px::CompileRuntimeIr(document);
    for (const auto& error : program.errors) std::cerr << error << '\n';
    assert(program.errors.empty());
    assert(program.code.front().type == "chapter");
    assert(program.code.front().sourceId == "scene");
    assert(program.code[1].type == "var");
    assert(program.code[2].type == "branch");
    assert(program.code[2].FindTyped("expression") != nullptr);
    bool foundMappedLine = false;
    for (const auto& command : program.code) {
        if (command.Get("value") == "你還記得。") {
            foundMappedLine = true;
            assert(command.line == 47);
            assert(command.sourceId == "say-true");
        }
    }
    assert(foundMappedLine);
    assert(program.LabelIndex("@if-else-if") >= 0);
    assert(program.LabelIndex("@if-end-if") >= 0);
    assert(program.code.back().type == "wait");
    assert(program.code.back().Get("ms") == "400");
    assert(program.code[program.code.size() - 2].type == "action");
    assert(program.code[program.code.size() - 2].Get("value").find("demo.typed") !=
           std::string::npos);
    const auto* actionArguments =
        program.code[program.code.size() - 2].FindTyped("arguments");
    const auto* actionObject = actionArguments ? actionArguments->AsObject() : nullptr;
    assert(actionObject != nullptr);
    assert(actionObject->at("amount").TryGet<std::int64_t>() != nullptr);
    assert(*actionObject->at("amount").TryGet<std::int64_t>() == 2);
    assert(actionObject->at("asset").TryGet<std::string>() != nullptr);
    assert(*actionObject->at("asset").TryGet<std::string>() ==
           "asset:33333333-3333-4333-8333-333333333333");

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

    px::sdk::RuntimeIrDocument typedCommandDocument;
    typedCommandDocument.documentId = "typed-command";
    typedCommandDocument.committedRevision = 3;
    typedCommandDocument.operations = {
        Operation(
            "typed-command", "customNode",
            {{"type", "game.setMood"},
             {"value",
              R"({"mood":"tense","strength":0.75})"}}),
        Operation(
            "typed-command-composite", "customNode",
            {{"type", "game.setMood"},
             {"value",
              R"({"strength":0.5,"asset":{"uuid":"11111111-1111-4111-8111-111111111111","kind":"uiImage","name":"Portrait"},"target":"22222222-2222-4222-8222-222222222222","offset":[12,-4],"bounds":[0,0,100,50],"tint":[255,128,64,255],"tags":["story","night"],"metadata":{"nested":{"keep":true}}})"}}),
        Operation("opaque-command", "customNode",
                  {{"type", "removed.command"},
                   {"value", R"({"future":{"keep":true}})"}}),
    };
    const px::vn::Program typedCommandProgram = px::CompileRuntimeIr(typedCommandDocument);
    for (const auto& error : typedCommandProgram.errors) std::cerr << error << '\n';
    assert(typedCommandProgram.errors.empty());
    assert(typedCommandProgram.code.size() == 3);
    const auto& typedCommand = typedCommandProgram.code[0];
    assert(typedCommand.type == "game.setMood");
    assert(typedCommand.Get("mood") == "tense");
    assert(typedCommand.FindTyped("strength") &&
           *typedCommand.FindTyped("strength")->TryGet<double>() == 0.75);
    assert(typedCommand.FindTyped("enabled") &&
           *typedCommand.FindTyped("enabled")->TryGet<bool>());
    const auto& compositeCommand = typedCommandProgram.code[1];
    assert(compositeCommand.Get("mood") == "calm");
    assert(compositeCommand.FindTyped("enabled") &&
           *compositeCommand.FindTyped("enabled")->TryGet<bool>());
    const auto* resource = compositeCommand.FindTyped("asset");
    assert(resource && resource->TryGet<px::ResourceRefValue>() &&
           resource->TryGet<px::ResourceRefValue>()->id.ToString() ==
               "11111111-1111-4111-8111-111111111111");
    assert(compositeCommand.FindTyped("target") &&
           compositeCommand.FindTyped("target")->TryGet<px::Uuid>() &&
           compositeCommand.FindTyped("target")->TryGet<px::Uuid>()->ToString() ==
               "22222222-2222-4222-8222-222222222222");
    assert(compositeCommand.FindTyped("offset") &&
           compositeCommand.FindTyped("offset")->TryGet<px::Vec2>() &&
           compositeCommand.FindTyped("offset")->TryGet<px::Vec2>()->x == 12.0f &&
           compositeCommand.FindTyped("offset")->TryGet<px::Vec2>()->y == -4.0f);
    assert(compositeCommand.FindTyped("bounds") &&
           compositeCommand.FindTyped("bounds")->TryGet<px::Rect>() &&
           compositeCommand.FindTyped("bounds")->TryGet<px::Rect>()->w == 100.0f);
    assert(compositeCommand.FindTyped("tint") &&
           compositeCommand.FindTyped("tint")->TryGet<px::Color>() &&
           compositeCommand.FindTyped("tint")->TryGet<px::Color>()->g == 128);
    assert(compositeCommand.FindTyped("tags") &&
           compositeCommand.FindTyped("tags")->AsArray() &&
           compositeCommand.FindTyped("tags")->AsArray()->size() == 2);
    assert(compositeCommand.FindTyped("metadata") &&
           compositeCommand.FindTyped("metadata")->AsObject() &&
           compositeCommand.FindTyped("metadata")->AsObject()->contains("nested"));
    assert(typedCommandProgram.code[2].type == "removed.command");
    assert(typedCommandProgram.code[2].Get("value") ==
           R"({"future":{"keep":true}})");

    px::sdk::RuntimeIrDocument invalidTypedCommandDocument;
    invalidTypedCommandDocument.documentId = "invalid-typed-command";
    invalidTypedCommandDocument.committedRevision = 1;
    invalidTypedCommandDocument.operations = {
        Operation("invalid", "customNode",
                  {{"type", "game.setMood"},
                   {"value", R"({"mood":"removed","strength":2,"extra":true})"}}),
    };
    const px::vn::Program invalidTypedCommandProgram =
        px::CompileRuntimeIr(invalidTypedCommandDocument);
    assert(!invalidTypedCommandProgram.errors.empty());

    px::sdk::RuntimeIrDocument parityDocument;
    parityDocument.documentId = "narrative-parity";
    parityDocument.committedRevision = 9;
    parityDocument.operations = {
        Operation("dialogue", "dialogue",
                  {{"character", "yuki"},
                   {"text", "Don't forget me."},
                   {"color", "255,240,230,255"},
                   {"outline", "0,0,0,255"},
                   {"speed", "36"},
                   {"effect", "shake"},
                   {"voice", "asset:voice-id"}}),
        Operation("character", "showCharacter",
                  {{"character", "yuki"},
                   {"sprite", "asset:sprite-id"},
                   {"expression", "smile"},
                   {"position", "1"},
                   {"x", "-24"},
                   {"y", "12"},
                   {"scale", "1.25"}}),
        Operation("add", "setVariable", {{"name", "affection"}, {"add", "2"}}),
        Operation("transition", "background",
                  {{"asset", "asset:background-id"},
                   {"rule", "asset:rule-id"},
                   {"duration", "0.6s"},
                   {"vague", "64"}}),
        Operation("animate", "timeline",
                  {{"mode", "animate"},
                   {"target", "yuki"},
                   {"x", "20"},
                   {"y", "0"},
                   {"scale", "1"},
                   {"alpha", "192"},
                   {"duration", "600ms"},
                   {"ease", "outCubic"},
                   {"wait", "true"}}),
        Operation("route", "ui", {{"route", "settings"}, {"operation", "modal"}}),
    };
    const px::vn::Program parityProgram = px::CompileRuntimeIr(parityDocument);
    for (const auto& error : parityProgram.errors) std::cerr << error << '\n';
    const auto check = [](const bool condition, const char* message) {
        if (!condition) std::cerr << "Narrative parity adapter failure: " << message << '\n';
        return condition;
    };
    if (!check(parityProgram.errors.empty(), "program must compile") ||
        !check(parityProgram.code.size() == 6, "expected six commands") ||
        !check(parityProgram.code[0].type == "say", "dialogue command") ||
        !check(parityProgram.code[0].Get("char") == "yuki", "dialogue character") ||
        !check(parityProgram.code[0].Get("voice") == "asset:voice-id", "dialogue voice") ||
        !check(parityProgram.code[0].Get("effect") == "shake", "dialogue effect") ||
        !check(parityProgram.code[1].type == "char", "character command") ||
        !check(parityProgram.code[1].Get("file") == "asset:sprite-id", "sprite override") ||
        !check(parityProgram.code[1].Get("pos") == "1", "position slot") ||
        !check(parityProgram.code[2].type == "var", "variable command") ||
        !check(parityProgram.code[2].FindTyped("add") != nullptr, "typed add") ||
        !check(parityProgram.code[3].type == "bg", "transition command") ||
        !check(parityProgram.code[3].Get("time") == "600", "transition duration") ||
        !check(parityProgram.code[3].Get("vague") == "64", "transition band") ||
        !check(parityProgram.code[4].type == "anim", "actor animation command") ||
        !check(parityProgram.code[4].Get("alpha") == "192", "animation alpha") ||
        !check(parityProgram.code[4].Get("wait") == "true", "animation wait") ||
        !check(parityProgram.code[5].type == "route", "route command") ||
        !check(parityProgram.code[5].Get("route") == "settings", "route id") ||
        !check(parityProgram.code[5].Get("operation") == "modal", "route operation")) {
        return 1;
    }

    // Keep this list mechanically tied to the published schema.  Adding a
    // RuntimeIR kind without an executable lowering case must make CI red.
    std::ifstream schemaInput(PRISMATIX_RUNTIME_IR_SCHEMA_PATH,
                              std::ios::binary);
    assert(schemaInput.good());
    nlohmann::json schema;
    schemaInput >> schema;
    std::set<std::string> schemaKinds;
    for (const auto& kind : schema.at("properties").at("operations")
                                .at("items").at("properties").at("kind")
                                .at("enum")) {
        schemaKinds.insert(kind.get<std::string>());
    }
    const std::set<std::string> exercisedKinds{
        "scene",          "fragment",       "callFragment", "return",
        "endStory",       "dialogue",       "narration",    "choiceOption",
        "background",     "showCharacter",  "hideCharacter", "voice",
        "bgm",            "soundEffect",    "setVariable",  "condition",
        "else",           "endCondition",   "label",        "jump",
        "wait",           "timeline",       "effect",       "ui",
        "customNode",     "camera",
    };
    assert(schemaKinds == exercisedKinds);

    px::sdk::RuntimeIrDocument allKindsDocument;
    allKindsDocument.documentId = "all-runtime-kinds";
    allKindsDocument.committedRevision = 1;
    allKindsDocument.operations = {
        Operation("all-scene", "scene", {{"title", "Conformance"}}),
        Operation("all-dialogue", "dialogue", {{"speaker", "A"}, {"text", "D"}}),
        Operation("all-narration", "narration", {{"text", "N"}}),
        Operation("all-choice", "choiceOption", {{"text", "Continue"}, {"target", "end"}}),
        Operation("all-background", "background", {{"asset", "bg.png"}}),
        Operation("all-show", "showCharacter", {{"character", "hero"}, {"sprite", "hero.png"}}),
        Operation("all-hide", "hideCharacter", {{"character", "hero"}}),
        Operation("all-voice", "voice", {{"asset", "voice.ogg"}}),
        Operation("all-bgm", "bgm", {{"asset", "music.ogg"}}),
        Operation("all-se", "soundEffect", {{"asset", "click.wav"}}),
        Operation("all-set", "setVariable", {{"name", "flag"}, {"value", "true"}}),
        Operation("all-if", "condition", {{"expression", "true"}}),
        Operation("all-if-body", "narration", {{"text", "yes"}}),
        Operation("all-else", "else"),
        Operation("all-else-body", "narration", {{"text", "no"}}),
        Operation("all-endif", "endCondition"),
        Operation("all-label", "label", {{"target", "end"}}),
        Operation("all-jump", "jump", {{"target", "end"}}),
        Operation("all-wait", "wait", {{"duration", "1ms"}}),
        Operation("all-timeline-asset", "timeline", {{"timeline", "intro.pxanim"}}),
        Operation("all-timeline-inline", "timeline", {{"mode", "animate"}, {"target", "hero"}, {"x", "4"}, {"duration", "1ms"}}),
        Operation("all-effect", "effect", {{"value", "fade"}}),
        Operation("all-ui", "ui", {{"route", "title"}, {"operation", "replace"}}),
        Operation("all-custom", "customNode", {{"type", "nvl"}, {"value", "{}"}}),
        Operation("all-camera", "camera", {{"value", "pan"}}),
        Operation("all-call", "callFragment", {{"target", "fragment"}}),
        Operation("all-end-story", "endStory"),
        Operation("all-fragment", "fragment", {{"target", "fragment"}}),
        Operation("all-return", "return"),
    };
    const px::vn::Program allKindsProgram =
        px::CompileRuntimeIr(allKindsDocument);
    for (const auto& error : allKindsProgram.errors) std::cerr << error << '\n';
    assert(allKindsProgram.errors.empty());
    assert(std::count_if(allKindsProgram.code.begin(), allKindsProgram.code.end(),
                         [](const auto& command) {
                             return command.type == "screen_effect";
                         }) == 2);
    assert(std::none_of(allKindsProgram.code.begin(), allKindsProgram.code.end(),
                        [](const auto& command) {
                            return command.type == "camera";
                        }));

    px::sdk::RuntimeIrDocument unknownDocument;
    unknownDocument.documentId = "unknown-operation";
    unknownDocument.committedRevision = 1;
    unknownDocument.operations = {
        Operation("structural", "sequence"),
    };
    const px::vn::Program unknownProgram = px::CompileRuntimeIr(unknownDocument);
    assert(!unknownProgram.errors.empty());
    assert(unknownProgram.errors.front().find("unknown or structural") !=
           std::string::npos);
    return 0;
}
