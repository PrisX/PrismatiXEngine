#include "Engine/Session/RuntimeIrAdapter.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Text/Typography.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <optional>
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
        Operation("set", "setVariable",
                  {{"name", "affection"}, {"value", "3"},
                   {"scope", "profile"}, {"type", "integer"}}),
        Operation(
            "if", "condition",
            {{"expression",
              R"({"kind":"binary","op":"and","left":{"kind":"binary","op":"greater-equal","left":{"kind":"variable","name":"affection"},"right":{"kind":"literal","value":3}},"right":{"kind":"unary","op":"not","operand":{"kind":"binary","op":"less","left":{"kind":"variable","name":"affection"},"right":{"kind":"literal","value":3}}}})"}}),
        Operation("say-true", "dialogue",
                  {{"speaker", "雪"}, {"text", "你還記得。"}}, 47),
        Operation("literal-speaker", "dialogue",
                  {{"speaker", "村雨"}, {"character", ""},
                   {"text", "早安，主人。"}}),
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
    const auto* profileScope = program.code[1].FindTyped("scope");
    assert(profileScope && profileScope->TryGet<std::string>() &&
           *profileScope->TryGet<std::string>() == "profile");
    const auto* profileValue = program.code[1].FindTyped("value");
    assert(profileValue && profileValue->TryGet<std::int64_t>() &&
           *profileValue->TryGet<std::int64_t>() == 3);
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
    const auto literalSay = std::find_if(
        program.code.begin(), program.code.end(), [](const auto& command) {
            return command.sourceId == "literal-speaker";
        });
    assert(literalSay != program.code.end());
    assert(literalSay->type == "say");
    assert(literalSay->Get("speaker") == "村雨");
    assert(literalSay->Get("char").empty());
    assert(literalSay->Get("value") == "早安，主人。");
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

    px::sdk::RuntimeIrDocument authoredLabelDocument;
    authoredLabelDocument.documentId = "authored-label-alias";
    authoredLabelDocument.committedRevision = 1;
    auto authoredLabel = Operation(
        "replay-label", "label", {{"target", "pxscene.label.1234567890abcdef"}});
    authoredLabel.text = "*replay.yoshino.after";
    authoredLabelDocument.operations = {authoredLabel};
    const px::vn::Program authoredLabelProgram =
        px::CompileRuntimeIr(authoredLabelDocument);
    assert(authoredLabelProgram.errors.empty());
    assert(authoredLabelProgram.LabelIndex("@pxscene.label.1234567890abcdef") >= 0);
    assert(authoredLabelProgram.LabelIndex("replay.yoshino.after") ==
           authoredLabelProgram.LabelIndex("@pxscene.label.1234567890abcdef"));

    px::sdk::RuntimeIrDocument typedVariableDocument;
    typedVariableDocument.documentId = "typed-variables";
    typedVariableDocument.committedRevision = 1;
    typedVariableDocument.operations = {
        Operation("number", "setVariable",
                  {{"name", "ratio"}, {"value", "5"},
                   {"scope", "session"}, {"type", "number"}}),
        Operation("empty", "setVariable",
                  {{"name", "route"}, {"value", ""},
                   {"scope", "profile"}, {"type", "string"}}),
    };
    const px::vn::Program typedVariableProgram =
        px::CompileRuntimeIr(typedVariableDocument);
    assert(typedVariableProgram.errors.empty());
    assert(typedVariableProgram.code.size() == 2);
    assert(typedVariableProgram.code[0].FindTyped("value") &&
           typedVariableProgram.code[0].FindTyped("value")->TryGet<double>());
    assert(typedVariableProgram.code[1].FindTyped("value") &&
           typedVariableProgram.code[1].FindTyped("value")->TryGet<std::string>() &&
           typedVariableProgram.code[1].FindTyped("value")
               ->TryGet<std::string>()->empty());

    px::sdk::RuntimeIrDocument invalidVariableDocument;
    invalidVariableDocument.documentId = "invalid-variables";
    invalidVariableDocument.committedRevision = 1;
    invalidVariableDocument.operations = {
        Operation("bad-scope", "setVariable",
                  {{"name", "flag"}, {"value", "true"},
                   {"scope", "temporary"}, {"type", "boolean"}}),
        Operation("bad-value", "setVariable",
                  {{"name", "score"}, {"value", "3.5"},
                   {"scope", "profile"}, {"type", "integer"}}),
    };
    const px::vn::Program invalidVariableProgram =
        px::CompileRuntimeIr(invalidVariableDocument);
    assert(invalidVariableProgram.code.empty());
    assert(invalidVariableProgram.errors.size() == 2);

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
                    {"scale", "1.25"},
                    {"transition", "false"}}),
        Operation("character-move", "timeline",
                  {{"mode", "move"},
                   {"target", "yuki"},
                   {"position", "3"},
                   {"x", "-48"},
                   {"y", "32"},
                   {"scale", "1"},
                   {"duration", "450ms"},
                   {"ease", "easeInOut"},
                   {"wait", "true"}}),
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
        !check(parityProgram.code.size() == 7, "expected seven commands") ||
        !check(parityProgram.code[0].type == "say", "dialogue command") ||
        !check(parityProgram.code[0].Get("char") == "yuki", "dialogue character") ||
        !check(parityProgram.code[0].Get("voice") == "asset:voice-id", "dialogue voice") ||
        !check(parityProgram.code[0].Get("effect") == "shake", "dialogue effect") ||
        !check(parityProgram.code[1].type == "char", "character command") ||
        !check(parityProgram.code[1].Get("file") == "asset:sprite-id", "sprite override") ||
         !check(parityProgram.code[1].Get("transition") == "false", "explicit instant character swap") ||
        !check(parityProgram.code[1].Get("pos") == "1", "position slot") ||
        !check(parityProgram.code[2].type == "char_move", "character move command") ||
        !check(parityProgram.code[2].Get("pos") == "3", "move position slot") ||
        !check(parityProgram.code[2].Get("x") == "-48", "move horizontal offset") ||
        !check(parityProgram.code[2].Get("duration") == "450", "move duration") ||
        !check(parityProgram.code[2].Get("wait") == "true", "move wait policy") ||
        !check(parityProgram.code[3].type == "var", "variable command") ||
        !check(parityProgram.code[3].FindTyped("add") != nullptr, "typed add") ||
        !check(parityProgram.code[4].type == "bg", "transition command") ||
        !check(parityProgram.code[4].Get("time") == "600", "transition duration") ||
        !check(parityProgram.code[4].Get("vague") == "64", "transition band") ||
        !check(parityProgram.code[5].type == "anim", "actor animation command") ||
        !check(parityProgram.code[5].Get("alpha") == "192", "animation alpha") ||
        !check(parityProgram.code[5].Get("wait") == "true", "animation wait") ||
        !check(parityProgram.code[6].type == "route", "route command") ||
        !check(parityProgram.code[6].Get("route") == "settings", "route id") ||
        !check(parityProgram.code[6].Get("operation") == "modal", "route operation")) {
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
        "background",     "showCharacter",  "hideCharacter", "hideCharacters", "voice",
        "bgm",            "stopBgm",        "soundEffect",  "ambience",
        "stopAmbience",   "video",          "setVariable",    "condition",
        "else",           "endCondition",   "label",        "jump",
        "wait",           "timeline",       "effect",       "shake",        "ui",
        "hudVisibility",  "unlock",         "customNode",   "camera",
        "actorAction",    "stopSoundEffect", "stopVoice",
        "image",          "hideImage",      "transition",
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
        Operation("all-show-auto", "showCharacter", {{"character", "hero"},
            {"sprite", "hero-expression.pxportrait"}, {"transition", "auto"}}),
        Operation("all-hide", "hideCharacter", {{"character", "hero"}}),
        Operation("all-hide-characters", "hideCharacters", {{"transition", "false"}}),
        Operation("all-voice", "voice", {{"asset", "voice.ogg"}}),
        Operation("all-bgm", "bgm", {{"asset", "music.ogg"},
                                      {"intro", "music-intro.ogg"},
                                      {"fade", "150ms"}}),
        Operation("all-bgm-once", "bgm", {{"asset", "sting.ogg"},
                                           {"loop", "false"}}),
        Operation("all-stop-bgm", "stopBgm", {{"fade", "100ms"}}),
        Operation("all-se", "soundEffect", {{"asset", "click.wav"}}),
        Operation("all-ambience", "ambience", {{"asset", "wind.ogg"},
                                                {"fade", "50ms"}, {"loop", "false"}}),
        Operation("all-stop-ambience", "stopAmbience", {{"fade", "25ms"}}),
        Operation("all-video", "video", {{"asset", "op.mp4"}, {"skippable", "true"}}),
        Operation("all-set", "setVariable", {{"name", "flag"}, {"value", "true"}}),
        Operation("all-if", "condition", {{"expression", "true"}}),
        Operation("all-if-body", "narration", {{"text", "yes"}}),
        Operation("all-else", "else"),
        Operation("all-else-body", "narration", {{"text", "no"}}),
        Operation("all-endif", "endCondition"),
        Operation("all-label", "label", {{"target", "end"}}),
        Operation("all-jump", "jump", {{"target", "end"}}),
        Operation("all-wait", "wait", {{"duration", "1ms"}}),
        Operation("all-timeline-asset", "timeline", {{"timeline", "intro.pxanim"},
                                                    {"await", "true"}, {"speed", "1.25"}}),
        Operation("all-stage-motion", "timeline", {{"mode", "stage-motion"},
            {"timeline", "intro.pxmotion"}, {"await", "true"}, {"speed", "1.5"}}),
        Operation("all-stage-motion-stop", "timeline", {{"mode", "stage-motion-stop"}}),
        Operation("all-timeline-inline", "timeline", {{"mode", "animate"}, {"target", "hero"}, {"x", "4"}, {"duration", "1ms"}}),
        Operation("all-effect", "effect", {{"value", "fade"}}),
        Operation("all-shake", "shake", {{"duration", "250ms"}, {"amplitude", "15"}}),
        Operation("all-ui", "ui", {{"route", "title"}, {"operation", "replace"}}),
        Operation("all-hud", "hudVisibility", {{"visible", "false"}}),
        Operation("all-act", "actorAction", {{"character", "hero"}, {"preset", "nod"},
            {"size", "8"}, {"time", "150ms"}, {"loop", "2"}, {"ease", "out"}}),
        Operation("all-stop-se", "stopSoundEffect", {{"fade", "200ms"}}),
        Operation("all-stop-voice", "stopVoice"),
        Operation("all-unlock", "unlock", {{"kind", "cg"}, {"id", "ev101a"}}),
        Operation("all-custom", "customNode", {{"type", "nvl"}, {"value", "{}"}}),
        Operation("all-camera", "camera", {{"value", "pan"}}),
        Operation("all-image", "image", {{"name", "cg"}, {"asset", "cg.png"},
            {"x", "-10"}, {"zoom", "0.5"}, {"anchor", "10,20"}, {"alpha", "128"},
            {"z", "9"}, {"duration", "300ms"}, {"fromAlpha", "0"}}),
        Operation("all-hide-image", "hideImage", {{"name", "*"}, {"duration", "1s"}}),
        Operation("all-transition", "transition", {{"duration", "0.5s"},
            {"rule", "rule.png"}, {"vague", "40"}, {"wait", "false"}}),
        Operation("all-call", "callFragment", {{"target", "fragment"}}),
        Operation("all-end-story", "endStory"),
        Operation("all-fragment", "fragment", {{"target", "fragment"}}),
        Operation("all-return", "return"),
    };
    const px::vn::Program allKindsProgram =
        px::CompileRuntimeIr(allKindsDocument);
    for (const auto& error : allKindsProgram.errors) std::cerr << error << '\n';
    assert(allKindsProgram.errors.empty());
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "bgm" && command.Get("intro") == "music-intro.ogg" &&
               command.Get("fade") == "150";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "bgm" && command.Get("file") == "sting.ogg" &&
               command.Get("loop") == "false";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "ambience" && command.Get("loop") == "false" &&
               command.Get("fade") == "50";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "animation" && command.Get("clip") == "intro.pxanim" &&
               command.Get("await") == "true" && command.Get("speed") == "1.25";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "stage_motion" &&
               command.Get("path") == "intro.pxmotion" &&
               command.Get("await") == "true" && command.Get("speed") == "1.5";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "stage_motion_stop";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "hud_visibility" && command.Get("visible") == "false";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "actor_action" && command.Get("id") == "hero" &&
               command.Get("preset") == "nod" && command.Get("time") == "150" &&
               command.Get("loop") == "2";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "stopse" && command.Get("fade") == "200";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "stopvoice";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "layer" && command.Get("name") == "cg" &&
               command.Get("anchor") == "10,20" && command.Get("time") == "300" &&
               command.Get("from_alpha") == "0";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "layer_clear" && command.Get("name") == "*" &&
               command.Get("time") == "1000";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "screen_transition" && command.Get("time") == "500" &&
               command.Get("vague") == "40" && command.Get("wait") == "false";
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "char" && command.Get("transitionMode") == "auto" &&
               !command.Has("transition");
    }));
    assert(std::ranges::any_of(allKindsProgram.code, [](const auto& command) {
        return command.type == "char_clear_all" && command.Get("transition") == "false";
    }));
    assert(px::vn::CommandRegistry::Builtins().Find("char_clear_all") != nullptr);
    assert(std::count_if(allKindsProgram.code.begin(), allKindsProgram.code.end(),
                         [](const auto& command) {
                             return command.type == "screen_effect";
                         }) == 2);
    assert(std::none_of(allKindsProgram.code.begin(), allKindsProgram.code.end(),
                        [](const auto& command) {
                            return command.type == "camera";
                        }));

    px::io::VFS clearVfs;
    px::audio::AudioEngine clearAudio(clearVfs);
    px::graphics::AssetCache clearAssets(nullptr, clearVfs);
    px::graphics::Renderer2D clearRenderer(nullptr, clearAssets);
    px::vn::Stage clearStage(clearRenderer, clearAssets);
    px::vn::Dialogue clearDialogue;
    px::vn::VariableStore clearVariables;
    px::vn::Backlog clearBacklog;
    px::vn::VM clearVm(clearVfs, clearAudio, clearStage, clearDialogue,
                       clearVariables, clearBacklog);
    clearStage.SetBackground("Assets/background.webp", false);
    clearStage.SetCharacter("hero", "Assets/hero.webp", 2, false);
    clearStage.SetCharacter("friend", "Assets/friend.webp", 3, false);
    px::sdk::RuntimeIrDocument clearDocument;
    clearDocument.documentId = "clear-all-runtime";
    clearDocument.operations = {
        Operation("clear-all", "hideCharacters", {{"transition", "false"}}),
        Operation("clear-end", "endStory"),
    };
    assert(clearVm.LoadCompiledProgram(px::CompileRuntimeIr(clearDocument),
                                       "memory://clear-all-runtime"));
    assert(clearStage.CaptureState().actors.empty());
    assert(clearStage.BackgroundPath() == "Assets/background.webp");

    // An expression-only show keeps a visible actor's placement; an explicit
    // position is a fresh placement with default offsets and scale.
    clearStage.SetCharacter("hero", "Assets/hero.webp", 1, false, 12.0f, -4.0f, 0.8f);
    px::sdk::RuntimeIrDocument placementDocument;
    placementDocument.documentId = "placement-runtime";
    placementDocument.operations = {
        Operation("keep", "showCharacter", {{"character", "hero"},
            {"sprite", "Assets/hero-smile.webp"}, {"transition", "false"}}),
        Operation("placement-end", "endStory"),
    };
    assert(clearVm.LoadCompiledProgram(px::CompileRuntimeIr(placementDocument),
                                       "memory://placement-runtime"));
    const auto kept = clearStage.FindCharacterPlacement("hero");
    assert(kept && kept->slot == 1 && kept->offsetX == 12.0f &&
           kept->offsetY == -4.0f && kept->scale == 0.8f);
    placementDocument.operations.front().arguments["position"] = "3";
    assert(clearVm.LoadCompiledProgram(px::CompileRuntimeIr(placementDocument),
                                       "memory://placement-runtime-moved"));
    const auto moved = clearStage.FindCharacterPlacement("hero");
    assert(moved && moved->slot == 3 && moved->offsetX == 0.0f && moved->scale == 1.0f);
    clearStage.ClearCharacters(false);

    // Relative actions return to rest, loop until stopped, and survive saves.
    {
        bool finished = false;
        const auto nod = px::vn::ExpandActorAction({"nod", 0.0f, 100, 1, ""});
        assert(nod && nod->keys.size() == 3);
        const auto peak = px::vn::SampleActorAction(*nod, 0.1, finished);
        assert(!finished && peak.y == 10.0f && peak.x == 0.0f);
        const auto rest = px::vn::SampleActorAction(*nod, 10.0, finished);
        assert(finished && rest.y == 0.0f);
        // いいえ (loop 1): left, right, left, then back to rest, as image_act.lua.
        const auto shake = px::vn::ExpandActorAction({"shake-head", 0.0f, 100, 1, ""});
        assert(shake && shake->keys.size() == 4 && shake->keys[0].to.x == -12.0f &&
               shake->keys[1].to.x == 12.0f && shake->keys[2].to.x == -12.0f &&
               shake->keys[3].to.x == 0.0f);
        const auto floating = px::vn::ExpandActorAction({"float", 5.0f, 100, -1, "linear"});
        assert(floating && floating->loopFrom == 1);
        const auto later = px::vn::SampleActorAction(*floating, 100.05, finished);
        assert(!finished && std::abs(later.y) <= 5.0f);
        assert(!px::vn::ExpandActorAction({"moonwalk", 0.0f, 0, 1, ""}));
        assert(!px::vn::ExpandActorAction({"nod", 0.0f, 0, 0, ""}));

        clearStage.SetCharacter("hero", "Assets/hero.webp", 2, false);
        assert(!clearStage.PlayActorAction("nobody", {"nod", 0.0f, 0, 1, ""}));
        assert(clearStage.PlayActorAction("hero", {"float", 0.0f, 0, -1, ""}));
        clearStage.Update(0.25f);
        auto saved = clearStage.Snapshot();
        assert(saved.size() == 1 && saved.front().action &&
               saved.front().action->preset == "float" &&
               saved.front().actionElapsed > 0.2);
        clearStage.Restore(saved);
        assert(clearStage.Snapshot().front().action);
        assert(clearStage.PlayActorAction("hero", {"stop", 0.0f, 0, 1, ""}));
        clearStage.Update(1.0f);
        assert(!clearStage.Snapshot().front().action);
        clearStage.ClearCharacters(false);
    }

    // Image layers and the background play the same actions. Saved state
    // resumes every running action; a new image or background ends one.
    {
        using SavedLayer = px::vn::Stage::SavedLayer;
        const auto layerAction = [](const px::vn::Stage::RuntimeState& state) {
            const auto found = std::ranges::find(state.layers, std::string("cg4"),
                                                 &SavedLayer::name);
            return found == state.layers.end() ? std::optional<SavedLayer>{}
                                               : std::optional<SavedLayer>{*found};
        };
        clearStage.SetLayer("cg4", "Assets/cg.webp", 0.0f, 0.0f, 0.5f, 255, 4);
        px::sdk::RuntimeIrDocument actDocument;
        actDocument.documentId = "image-action-runtime";
        actDocument.operations = {
            Operation("bg-act", "actorAction", {{"image", "$bg"}, {"preset", "shake-head"},
                {"size", "10"}, {"time", "100ms"}, {"loop", "-1"}}),
            Operation("cg-act", "actorAction", {{"image", "cg4"}, {"preset", "float"},
                {"loop", "-1"}}),
            Operation("missing-act", "actorAction", {{"image", "cg9"}, {"preset", "float"}}),
            Operation("act-end", "endStory"),
        };
        const px::vn::Program actProgram = px::CompileRuntimeIr(actDocument);
        assert(actProgram.errors.empty());
        assert(std::ranges::any_of(actProgram.code, [](const auto& command) {
            return command.type == "actor_action" && command.Get("id") == "$bg" &&
                   command.Get("target") == "image" && command.Get("time") == "100";
        }));
        assert(clearVm.LoadCompiledProgram(actProgram, "memory://image-action-runtime"));
        assert(!clearStage.PlayImageAction("cg9", {"float", 0.0f, 0, 1, ""}));
        assert(!clearStage.PlayImageAction("cg4", {"moonwalk", 0.0f, 0, 1, ""}));
        clearStage.SetCharacter("hero", "Assets/hero.webp", 2, false);
        assert(clearStage.PlayActorAction("hero", {"nod", 0.0f, 0, -1, ""}));
        clearStage.Update(0.25f);

        const auto saved = clearStage.CaptureState();
        assert(saved.backgroundAction && saved.backgroundAction->preset == "shake-head" &&
               saved.backgroundAction->size == 10.0f && saved.backgroundAction->timeMs == 100 &&
               saved.backgroundActionElapsed > 0.2);
        const auto savedLayer = layerAction(saved);
        assert(savedLayer && savedLayer->action && savedLayer->action->preset == "float" &&
               savedLayer->actionElapsed > 0.2);
        assert(saved.actors.size() == 1 && saved.actors.front().action);
        assert(static_cast<bool>(clearStage.RestoreState(saved)));
        const auto restored = clearStage.CaptureState();
        assert(restored.backgroundAction &&
               restored.backgroundActionElapsed == saved.backgroundActionElapsed);
        const auto restoredLayer = layerAction(restored);
        assert(restoredLayer && restoredLayer->action &&
               restoredLayer->actionElapsed == savedLayer->actionElapsed);
        assert(restored.actors.size() == 1 && restored.actors.front().action &&
               restored.actors.front().action->preset == "nod");

        // Reframing the same image keeps its action; another image ends it.
        clearStage.SetLayerTransform("cg4", "Assets/cg.webp", 40.0f, 0.0f, 0.5f, 0.5f,
                                     0.0f, 255, 4);
        assert(layerAction(clearStage.CaptureState())->action);
        clearStage.SetLayer("cg4", "Assets/cg-night.webp", 0.0f, 0.0f, 0.5f, 255, 4);
        assert(!layerAction(clearStage.CaptureState())->action);
        assert(clearStage.PlayImageAction("cg4", {"sway", 0.0f, 0, 1, ""}));
        assert(clearStage.PlayImageAction("cg4", {"stop", 0.0f, 0, 1, ""}));
        clearStage.Update(1.0f);
        assert(!layerAction(clearStage.CaptureState())->action);
        clearStage.SetBackground("Assets/night.webp", false);
        assert(!clearStage.CaptureState().backgroundAction);

        clearStage.ClearLayer("cg4");
        clearStage.ClearCharacters(false);
        clearStage.SetBackground("Assets/background.webp", false);
    }

    // Inline dialogue styles: tags become runs over the plain text next to
    // ruby, take no typewriter step and stay in the backlog as markup.
    {
        const auto rich = px::text::ParseRubyMarkup(
            "ab[size=150%][b]CD[/b][/size][color=#FF000080]E[ruby=い]F[/ruby][/color][bogus]G");
        assert(rich.plain == "abCDEFG");
        assert(rich.ruby.size() == 1 && rich.ruby.front().prefix == "abCDE");
        assert(rich.runs.size() == 4);
        assert(rich.runs[0].byteStart == 0 && rich.runs[0].byteLength == 2 &&
               rich.runs[0].style.Plain());
        assert(rich.runs[1].byteLength == 2 && rich.runs[1].style.bold &&
               rich.runs[1].style.scale == 1.5f);
        assert(rich.runs[2].byteStart == 4 && rich.runs[2].byteLength == 2 &&
               rich.runs[2].style.color && rich.runs[2].style.color->r == 255 &&
               rich.runs[2].style.color->g == 0 && rich.runs[2].style.color->a == 0x80);
        assert(rich.runs[3].byteStart == 6 && rich.runs[3].style.Plain());
        assert(px::text::ParseRubyMarkup("[ruby=a]b[/ruby][br]c").runs.empty());
        assert(px::text::ParseRubyMarkup("[b][/b]x").runs.empty());
        // Relative sizes multiply, an absolute size resets the scale, and an
        // unclosed tag lasts to the end.
        const auto nested = px::text::ParseRubyMarkup(
            "[size=50%][size=200%]x[/size][size=30]y[/size]z[i]w");
        assert(nested.plain == "xyzw" && nested.runs.size() == 4);
        assert(nested.runs[0].style.Plain());
        assert(nested.runs[1].style.size == 30 && nested.runs[1].style.scale == 1.0f);
        assert(nested.runs[2].style.scale == 0.5f && !nested.runs[2].style.italic);
        assert(nested.runs[3].style.italic && nested.runs[3].byteStart == 3);
        // Closing tags need not nest strictly; a stray one changes nothing.
        const auto crossed = px::text::ParseRubyMarkup("[b][font=F.otf]x[/b]y[/font][/color]z");
        assert(crossed.runs.size() == 3 && crossed.runs[1].style.font == "F.otf" &&
               !crossed.runs[1].style.bold && crossed.runs[2].style.Plain());
        assert(px::text::ParseRubyMarkup("[color=red]x").runs.empty());
        assert(px::text::StyleTagLength("[b]x") == 3);
        assert(px::text::StyleTagLength("[/color]") == 8);
        assert(px::text::StyleTagLength("[bold]") == 0);
        assert(px::text::StyleTagLength("[size]") == 0);
        assert(px::text::StyleTagLength("[/b=1]") == 0);
        assert(px::text::StyledFontPath("F.otf", true, true) == "F.otf|bi");
        assert(px::text::StyledFontPath("F.otf", false, false) == "F.otf");

        px::vn::Dialogue styled;
        styled.SetText("A", "a[size=200%]bc[/size]d[b]", 10, {}, {});
        assert(styled.State().totalChars == 4);
        assert(styled.State().fullText == "a[size=200%]bc[/size]d[b]");
        styled.Update(1000);
        styled.Update(1015);
        assert(styled.State().displayText == "a");
        styled.Update(1020);
        assert(styled.State().displayText == "a[size=200%]b");
        styled.Update(1030);
        assert(styled.State().displayText == "a[size=200%]bc[/size]");
        styled.Update(1040);
        assert(styled.State().finished &&
               styled.State().displayText == "a[size=200%]bc[/size]d[b]");

        px::vn::Program styledLine;
        px::vn::Command say;
        say.type = "say";
        say.args = {{"speaker", "A"}, {"text", "[size=75%]quiet[/size] {w=100}[color=#102030]end"}};
        styledLine.code.push_back(std::move(say));
        assert(clearVm.LoadCompiledProgram(std::move(styledLine), "memory://styled-say"));
        assert(clearDialogue.State().fullText == "[size=75%]quiet[/size] [color=#102030]end");
        assert(!clearBacklog.Entries().empty() &&
               clearBacklog.Entries().back().text == clearDialogue.State().fullText);
    }

    // Sound effect channels reach the VM with their loop and fade.
    {
        px::sdk::RuntimeIrDocument seDocument;
        seDocument.documentId = "se-channels";
        seDocument.operations = {
            Operation("rain", "soundEffect", {{"asset", "Assets/rain.ogg"}, {"channel", "2"},
                {"loop", "true"}, {"fade", "500ms"}, {"volume", "40"}}),
            Operation("rain-stop", "stopSoundEffect", {{"channel", "2"}, {"fade", "300ms"}}),
            Operation("all-stop", "stopSoundEffect"),
        };
        const px::vn::Program seProgram = px::CompileRuntimeIr(seDocument);
        assert(seProgram.errors.empty());
        assert(seProgram.code.size() == 3);
        assert(seProgram.code[0].type == "se" && seProgram.code[0].Get("channel") == "2" &&
               seProgram.code[0].Get("loop") == "true" && seProgram.code[0].Get("fade") == "500");
        assert(seProgram.code[1].type == "stopse" && seProgram.code[1].Get("channel") == "2" &&
               seProgram.code[1].Get("fade") == "300");
        assert(seProgram.code[2].type == "stopse" && !seProgram.code[2].Has("channel"));
        // Headless audio accepts the calls and keeps no channel state.
        assert(clearVm.LoadCompiledProgram(seProgram, "memory://se-channels"));
        assert(clearAudio.CaptureState().seChannels.empty());
        px::audio::AudioEngine::RuntimeState invalid;
        invalid.seChannels.push_back({0, {}});
        assert(!clearAudio.ValidateState(invalid, false));
        invalid.seChannels = {{2, {}}, {2, {}}};
        assert(!clearAudio.ValidateState(invalid, false));
        invalid.seChannels = {{2, {.gain = 0.5f}}};
        assert(clearAudio.ValidateState(invalid, false));
    }

    px::vn::GameCatalog localizedCatalog;
    bool charactersDeclared = false;
    constexpr std::string_view localizedCharacterId =
        "11111111-1111-4111-8111-111111111111";
    const auto localizedCatalogStatus = localizedCatalog.LoadCharacterResources(
        R"({"format":"PrismatiXProject","schemaRevision":2,"assets":[],"characters":[{"id":"11111111-1111-4111-8111-111111111111","displayName":"凛","source":"Characters/rin.pxcharacter"}]})",
        [](std::string_view) -> std::optional<std::string> {
            return R"({"format":"PrismatiXCharacter","schemaRevision":2,"id":"11111111-1111-4111-8111-111111111111","displayName":"凛","displayNames":{"ja-JP":"リン"},"expressions":[]})";
        },
        [](std::string_view) { return false; }, charactersDeclared);
    assert(localizedCatalogStatus.IsOk());
    assert(charactersDeclared);
    clearVm.SetGameCatalog(localizedCatalog);
    const auto playSay = [&](const std::string_view locale,
                             const std::string_view explicitSpeaker = {}) {
        clearVm.SetCharacterLocale(std::string(locale));
        px::vn::Program lines;
        px::vn::Command say;
        say.type = "say";
        say.args = {{"char", std::string(localizedCharacterId)},
                    {"text", "Hello"}};
        if (!explicitSpeaker.empty())
            say.args.push_back({"speaker", std::string(explicitSpeaker)});
        lines.code.push_back(std::move(say));
        assert(clearVm.LoadCompiledProgram(std::move(lines), "memory://localized-say"));
        return clearDialogue.State().speaker;
    };
    assert(playSay("ja-JP") == "リン");
    assert(playSay("zh-TW") == "凛");
    assert(playSay("ja-JP", "Authored speaker") == "Authored speaker");

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
