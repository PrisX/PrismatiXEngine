#include <iostream>
#include <set>
#include <string>
#include <utility>

#include "Engine/Session/RuntimeAssetResolver.h"

namespace {

constexpr std::string_view kBackgroundId = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view kRuleId = "22222222-2222-4222-8222-222222222222";
constexpr std::string_view kSpriteId = "33333333-3333-4333-8333-333333333333";
constexpr std::string_view kVoiceId = "44444444-4444-4444-8444-444444444444";

std::string Manifest() {
    return R"({
  "format":"PrismatiXProject",
  "schemaRevision":2,
  "assets":[
    {"id":"11111111-1111-4111-8111-111111111111","kind":"background","source":"Assets/bg.png"},
    {"id":"22222222-2222-4222-8222-222222222222","kind":"transition","source":"Assets/rule.png"},
    {"id":"33333333-3333-4333-8333-333333333333","kind":"character","source":"Assets/sprite.png"},
    {"id":"44444444-4444-4444-8444-444444444444","kind":"voice","source":"Assets/voice.ogg"}
  ]
})";
}

struct Suite {
    bool passed = true;

    void Expect(const bool condition, const std::string& message) {
        if (condition) return;
        passed = false;
        std::cerr << "Runtime asset resolver failure: " << message << '\n';
    }
};

const px::ResourceRefValue* Resource(const px::vn::Command& command, const std::string& name) {
    const auto* value = command.FindTyped(name);
    return value ? value->TryGet<px::ResourceRefValue>() : nullptr;
}

}  // namespace

int main() {
    Suite suite;
    const std::set<std::string> existing{ "Assets/bg.png", "Assets/rule.png", "Assets/sprite.png", "Assets/voice.ogg" };
    const auto exists = [&existing](const std::string_view path) { return existing.contains(std::string(path)); };

    px::vn::Program program;
    px::vn::Command dialogue;
    dialogue.type = "say";
    dialogue.line = 10;
    dialogue.args.push_back({ "voice", "asset:" + std::string(kVoiceId) });
    program.code.push_back(std::move(dialogue));

    px::vn::Command character;
    character.type = "char";
    character.line = 11;
    character.args.push_back({ "file", "asset:" + std::string(kSpriteId) });
    program.code.push_back(std::move(character));

    px::vn::Command background;
    background.type = "bg";
    background.line = 12;
    background.args.push_back({ "file", "asset:" + std::string(kBackgroundId) });
    background.args.push_back({ "rule", "asset:" + std::string(kRuleId) });
    program.code.push_back(std::move(background));

    px::vn::Command custom;
    custom.type = "custom";
    custom.line = 13;
    custom.typedArgs["payload"] = px::Variant(px::VariantObject{ { "thumbnail", px::Variant("asset:" + std::string(kSpriteId)) } });
    program.code.push_back(std::move(custom));

    suite.Expect(px::UsesRuntimeAssetReferences(program), "asset tokens are detected before VM execution");
    auto resolved = px::ResolveRuntimeAssetReferences(program, Manifest(), exists, "Content/Runtime/story.pxir");
    suite.Expect(static_cast<bool>(resolved), "known UUIDs with existing files resolve atomically");
    if (resolved) {
        const auto& output = resolved.Value();
        suite.Expect(output.code[0].Get("voice") == "Assets/voice.ogg", "dialogue voice resolves to its runtime path");
        suite.Expect(output.code[1].Get("file") == "Assets/sprite.png", "character sprite resolves to its runtime path");
        suite.Expect(output.code[2].Get("file") == "Assets/bg.png" && output.code[2].Get("rule") == "Assets/rule.png", "background and transition rule resolve together");
        const auto* voice = Resource(output.code[0], "voice");
        const auto* sprite = Resource(output.code[1], "file");
        suite.Expect(voice && voice->id.ToString() == kVoiceId && voice->lastKnownPath == "Assets/voice.ogg", "resolved voice retains UUID identity in typed arguments");
        suite.Expect(sprite && sprite->id.ToString() == kSpriteId && sprite->lastKnownPath == "Assets/sprite.png", "resolved sprite retains UUID identity in typed arguments");
        const auto* payload = output.code[3].FindTyped("payload");
        const auto* object = payload ? payload->AsObject() : nullptr;
        const auto thumbnail = object ? object->find("thumbnail") : px::VariantObject::const_iterator{};
        const auto* nested = object && thumbnail != object->end() ? thumbnail->second.TryGet<px::ResourceRefValue>() : nullptr;
        suite.Expect(nested && nested->id.ToString() == kSpriteId && nested->lastKnownPath == "Assets/sprite.png", "nested typed action arguments use the same resolver");
    }

    px::vn::Program unknown;
    px::vn::Command missing;
    missing.type = "bg";
    missing.line = 47;
    missing.args.push_back({ "file", "asset:aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" });
    unknown.code.push_back(std::move(missing));
    const auto unknownResult = px::ResolveRuntimeAssetReferences(unknown, Manifest(), exists, "Content/Runtime/story.pxir");
    suite.Expect(
        !unknownResult && !unknownResult.Diagnostics().empty() && unknownResult.Diagnostics().front().code == "PXRUNTIME7317" && unknownResult.Diagnostics().front().source.line == 47 && unknownResult.Diagnostics().front().source.property == "file",
        "unknown UUID is a structured source-mapped failure"
    );
    suite.Expect(unknown.code.front().Get("file").starts_with("asset:"), "failed resolution does not partially mutate the caller program");

    px::vn::Program invalid;
    px::vn::Command invalidCommand;
    invalidCommand.type = "voice";
    invalidCommand.line = 5;
    invalidCommand.args.push_back({ "file", "asset:not-a-uuid" });
    invalid.code.push_back(std::move(invalidCommand));
    const auto invalidResult = px::ResolveRuntimeAssetReferences(std::move(invalid), Manifest(), exists, "Content/Runtime/story.pxir");
    suite.Expect(!invalidResult && !invalidResult.Diagnostics().empty() && invalidResult.Diagnostics().front().code == "PXRUNTIME7316", "malformed asset tokens are rejected");

    px::vn::Program missingFile;
    px::vn::Command missingFileCommand;
    missingFileCommand.type = "bgm";
    missingFileCommand.args.push_back({ "file", "asset:" + std::string(kVoiceId) });
    missingFile.code.push_back(std::move(missingFileCommand));
    const auto missingFileResult = px::ResolveRuntimeAssetReferences(std::move(missingFile), Manifest(), [](std::string_view) { return false; }, "Content/Runtime/story.pxir");
    suite.Expect(!missingFileResult && !missingFileResult.Diagnostics().empty() && missingFileResult.Diagnostics().front().code == "PXRUNTIME7318", "known UUID with a missing runtime file is rejected");

    px::vn::Program pathOnly;
    px::vn::Command legacyPath;
    legacyPath.type = "voice";
    legacyPath.args.push_back({ "file", "Content/Audio/legacy.ogg" });
    pathOnly.code.push_back(std::move(legacyPath));
    const auto pathOnlyResult = px::ResolveRuntimeAssetReferences(std::move(pathOnly), "", {}, "Content/Runtime/legacy.pxir");
    suite.Expect(pathOnlyResult && pathOnlyResult.Value().code.front().Get("file") == "Content/Audio/legacy.ogg", "path-only compatibility IR does not require a manifest lookup");

    return suite.passed ? 0 : 1;
}
