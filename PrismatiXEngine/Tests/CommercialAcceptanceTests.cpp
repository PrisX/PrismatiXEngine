#include "Engine/IO/Archive.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/IO/VFS.h"
#include "Engine/Lua/LuaHost.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/UI/Widgets.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Expression/Expression.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Engine/VN/Scenario/StoryMap.h"
#include "Engine/Text/Typography.h"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
}
void Check(const px::Status& status, const char* message) {
    Check(static_cast<bool>(status), message);
}

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("prismatix-tests-" + px::Uuid::Random().ToString());
    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void TestArchiveBoundsAndRoundTrip() {
    TempDirectory temp;
    const auto archivePath = temp.path / "valid.pdx";
    px::io::ArchiveWriter writer;
    writer.SetCompression(true);
    const px::io::Bytes expected{'P', 'r', 'i', 's', 'm', 'a'};
    writer.Add("Content/test.txt", expected);
    Check(writer.Write(archivePath.string()), "valid archive should be written");

    px::io::Archive archive;
    Check(archive.Open(archivePath.string()), "valid archive should open");
    Check(archive.Contains("Content/test.txt"), "archive should contain exact normalized path");
    const auto actual = archive.Read("Content/test.txt");
    Check(actual && *actual == expected, "archive payload should round-trip");
    Check(!archive.Contains("Content/other.txt"), "archive should reject missing path");

    const auto corruptPath = temp.path / "corrupt.pdx";
    {
        std::ofstream stream(corruptPath, std::ios::binary);
        stream.write("PDX3", 4);
    }
    px::io::Archive corrupt;
    Check(!corrupt.Open(corruptPath.string()), "truncated archive must be rejected");

    px::io::ArchiveWriter unsafe;
    unsafe.Add("../escape.txt", expected);
    Check(!unsafe.Write((temp.path / "unsafe.pdx").string()),
          "archive writer must reject traversal paths");
    const auto encryptedPath=temp.path/"encrypted.pdx";px::io::ArchiveWriter encrypted;const auto key=px::crypto::DeriveKey("test-key");encrypted.SetKey(key);encrypted.Add("Content/secure.txt",expected);Check(encrypted.Write(encryptedPath.string()),"authenticated archive should build");px::io::Archive secured;Check(secured.Open(encryptedPath.string(),&key)&&secured.Read("Content/secure.txt")==expected,"AES-GCM archive should decrypt and authenticate");{std::fstream stream(encryptedPath,std::ios::binary|std::ios::in|std::ios::out);stream.seekg(-1,std::ios::end);char byte=0;stream.read(&byte,1);byte^=0x40;stream.seekp(-1,std::ios::end);stream.write(&byte,1);}px::io::Archive tampered;Check(!tampered.Open(encryptedPath.string(),&key),"authenticated archive must reject a modified index");
}

void TestSaveValidation() {
    TempDirectory temp;
    px::progress::SaveSystem saves;
    saves.Configure(temp.path.string(), nullptr);

    px::progress::SaveSnapshot snapshot;
    snapshot.scriptPath = "Content/Scenario/start.pxscenario";
    snapshot.pc = 7;
    snapshot.chapter = "Chapter 1";
    snapshot.variables["affection"] = 3;
    snapshot.typedVariables["route"] = px::vn::Value("alice");
    snapshot.typedVariables["flags"] = px::vn::Value(
        px::vn::ValueMap{{"ending", px::vn::Value(true)},
                         {"scores", px::vn::Value(px::vn::ValueList{1, 2, 3})}});
    snapshot.persistentVariables.insert("affection");
    snapshot.vm.scriptPath = snapshot.scriptPath;
    snapshot.vm.pc = 8;
    snapshot.vm.state = px::vn::VMState::WaitingChoice;
    snapshot.vm.callStack.push_back({"Content/Scenario/prologue.pxscenario", 12});
    snapshot.vm.choices.push_back({"Stay", "stay"});
    snapshot.vm.speaker = "Alice";
    snapshot.vm.textEffect = "pulse";
    snapshot.vm.timerRemainingMs = 250;
    snapshot.dialogue.state.speaker = "Alice";
    snapshot.dialogue.state.fullText = "Hello";
    snapshot.dialogue.state.displayText = "Hel";
    snapshot.dialogue.state.currentChar = 3;
    snapshot.dialogue.state.totalChars = 5;
    snapshot.dialogue.state.effect = "pulse";
    snapshot.dialogue.state.effectProgress = 0.5f;
    snapshot.dialogue.speedMs = 42;
    snapshot.routes.stack = {"game", "settings"};
    snapshot.routes.modals = {"confirm"};
    snapshot.timelines.push_back({17, px::Uuid::FromName("PrismatiX.OfficialPreset.Text.fade"),
                                  0.2f, 1.0f, 0, true, true});
    px::animation::AnimationClip savedClip;savedClip.id=px::Uuid::FromName("save-custom-clip");
    savedClip.name="Custom/Save";savedClip.duration=1.0f;
    savedClip.tracks.push_back({{px::animation::TargetKind::Stage,"alice","alpha"},
                                {{0.0f,0.0,px::animation::Curve::Linear},{1.0f,1.0,px::animation::Curve::EaseOut}}});
    snapshot.animationClips.push_back(std::move(savedClip));
    snapshot.stage.background = "Content/Background/room.png";
    snapshot.stage.previousBackground = "Content/Background/hall.png";
    snapshot.stage.backgroundFade = 0.45f;
    snapshot.stage.cameraZoom = 1.15f;
    snapshot.stage.screenEffects["vignette"] = 0.3f;
    snapshot.stage.actors.push_back({"alice", "Content/Characters/alice.png", 2, 4.0f,
                                     -2.0f, 1.0f, {}, 180.0f, 255.0f, 0.0f,
                                     640.0f, 640.0f, false});
    px::vn::Stage::SavedTween tween; tween.target="alice"; tween.spec.hasX=true;
    tween.spec.x=24.0f; tween.elapsed=0.2f; tween.duration=0.6f;
    snapshot.stage.tweens.push_back(tween);
    snapshot.audio.music = {"Content/Audio/theme.ogg", true, true, 24000};
    px::lua::PendingCommandState pending; pending.command.type="demo.await";
    pending.command.args.push_back({"duration", "0.5"}); pending.yieldIndex=1;
    pending.waitKind="timer"; pending.remainingSeconds=0.25f;
    snapshot.luaPending.push_back(std::move(pending));
    Check(saves.Save(0, snapshot), "valid save should be written");
    const auto loaded = saves.Load(0);
    Check(loaded && loaded->pc == 7 && loaded->variables.at("affection") == 3,
          "valid save should round-trip");
    Check(loaded && loaded->persistentVariables.contains("affection") &&
              loaded->vm.state == px::vn::VMState::WaitingChoice &&
              loaded->vm.callStack.size() == 1 && loaded->vm.choices.size() == 1 &&
              loaded->dialogue.state.displayText == "Hel" && loaded->dialogue.speedMs == 42 &&
              loaded->typedVariables.at("route").TryGet<std::string>() &&
              loaded->typedVariables.at("flags").AsObject() &&
              loaded->routes.stack.size() == 2 && loaded->routes.modals.size() == 1 &&
              loaded->timelines.size() == 1 && loaded->timelines.front().awaiting &&
              loaded->animationClips.size() == 1 && loaded->animationClips.front().name == "Custom/Save" &&
              loaded->stage.backgroundFade == 0.45f && loaded->stage.actors.size() == 1 &&
              loaded->stage.tweens.size() == 1 && loaded->audio.music.playbackFrame == 24000 &&
              loaded->luaPending.size() == 1 && loaded->luaPending.front().yieldIndex == 1,
          "version 3 save should preserve exact VM, stage, audio, Lua, and variable state");

    px::progress::Json wrongType{{"version", 2}, {"variables", {{"affection", "high"}}}};
    Check(px::progress::SaveJson(saves.SlotPath(1), wrongType, nullptr),
          "wrong-type fixture should be written");
    Check(!saves.Load(1), "wrong-type save must fail without throwing");

    px::progress::Json removedV2{{"version", 2}, {"scriptPath", "removed"}};
    Check(px::progress::SaveJson(saves.SlotPath(3), removedV2, nullptr),
          "removed v2 fixture should be written");
    Check(!saves.Load(3), "version 2 saves must be rejected after the strict 3.0 cutover");

    px::progress::Json wrongVersion{{"version", 999}};
    Check(px::progress::SaveJson(saves.SlotPath(2), wrongVersion, nullptr),
          "wrong-version fixture should be written");
    Check(!saves.Load(2), "unsupported save version must be rejected");
    Check(!saves.Peek(2).exists, "unsupported save must not appear as a valid slot");
}

void TestTypedUIEventBinding() {
    px::resource::TypedDocument document;
    document.kind = px::resource::DocumentKind::Scene;
    document.id = px::Uuid::Random();
    document.type = "UIScene";
    px::resource::NodeRecord button;
    button.id = px::Uuid::Random();
    button.type = "Button";
    button.name = "Start";
    button.properties["events"] = px::VariantObject{{"activated",px::VariantObject{{"action",std::string("game.start")}}}};
    document.nodes.push_back(std::move(button));

    px::ui::FormatterRegistry formatters;
    const auto loaded = px::ui::InstantiateUIScene(document, nullptr, formatters);
    Check(static_cast<bool>(loaded), "typed EventBinding scene should load");
    if (loaded) {
        const auto* runtimeButton = dynamic_cast<const px::ui::Button*>(loaded.Value().root.get());
        Check(runtimeButton && runtimeButton->Command() == "game.start",
              "typed EventBinding should map to the runtime Button command");
    }
}

void TestDialogueEffects() {
    px::vn::Dialogue dialogue;
    dialogue.SetText("A", "Hello", 0, {}, {}, {}, "shake");
    dialogue.Update(100);
    dialogue.Update(350);
    Check(dialogue.State().effect == "shake" && dialogue.State().effectProgress >= 0.24f,
          "dialogue effect should remain animated after typewriter completion");

}

void TestCommandSchemaAndScenarioRoundTrip() {
    px::vn::scenario::ScenarioDocument scenario;
    scenario.id = px::Uuid::FromName("scenario-test");
    scenario.name = "Chapter 1";
    px::vn::scenario::ScenarioNode chapter{px::Uuid::FromName("chapter-node"), "chapter",
                                            {{"title", px::Variant("Chapter 1")}}};
    px::vn::scenario::ScenarioNode variable{px::Uuid::FromName("variable-node"), "var",
                                             {{"name", px::Variant("affection")},
                                              {"add", px::Variant(2)}}};
    scenario.entry = chapter.id;
    scenario.nodes = {chapter, variable};
    scenario.edges.push_back({px::Uuid::FromName("scenario-edge"), chapter.id, "flow",
                              variable.id, "in"});
    const auto validation = px::vn::scenario::ValidateScenario(scenario);
    Check(validation.Valid(), "strict typed Scenario should satisfy the shared command schema");
    const std::string first = px::vn::scenario::WriteScenario(scenario);
    const auto reparsed = px::vn::scenario::ParseScenario(first, "memory.pxscenario");
    Check(static_cast<bool>(reparsed), "Scenario document should parse after serialization");
    if (reparsed) {
        Check(px::vn::scenario::WriteScenario(reparsed.Value()) == first,
              "Scenario serialization should be deterministic");
        Check(reparsed.Value().nodes.size() == 2 &&
                  reparsed.Value().nodes[1].parameters.at("add").Type() == px::VariantType::Integer,
              "Scenario should preserve typed command parameters");
    }
    const auto program = px::vn::scenario::CompileScenario(scenario);
    Check(program.errors.empty(), "strict Scenario should compile directly to VM instructions");
}

void TestTypedExpressions() {
    const auto expression = px::vn::Expression::Binary(
        px::vn::ExpressionOperator::And,
        px::vn::Expression::Binary(px::vn::ExpressionOperator::GreaterEqual,
                                   px::vn::Expression::Variable("affection"),
                                   px::vn::Expression::Literal(3)),
        px::vn::Expression::Binary(px::vn::ExpressionOperator::Equal,
                                   px::vn::Expression::Variable("route"),
                                   px::vn::Expression::Literal("alice")));
    const auto variables = [](std::string_view name) -> std::optional<px::vn::Value> {
        if (name == "affection") return px::vn::Value(4);
        if (name == "route") return px::vn::Value("alice");
        return std::nullopt;
    };
    const auto evaluated = px::vn::EvaluateExpression(expression, variables);
    Check(evaluated && evaluated.Value().TryGet<bool>() &&
              *evaluated.Value().TryGet<bool>(),
          "typed expression should evaluate bool, number, and string operands");

    const px::vn::Value encoded = px::vn::ExpressionToValue(expression);
    const auto decoded = px::vn::ExpressionFromValue(encoded);
    const auto reevaluated = decoded ? px::vn::EvaluateExpression(decoded.Value(), variables)
                                     : px::Result<px::vn::Value>{};
    Check(decoded && reevaluated && reevaluated.Value() == evaluated.Value(),
          "expression AST should survive typed Value serialization");

    px::vn::scenario::ScenarioDocument expressionScenario;
    expressionScenario.id = px::Uuid::FromName("expression-scenario");
    px::vn::scenario::ScenarioNode condition;
    condition.id = px::Uuid::FromName("condition-node");
    condition.command = "branch";
    condition.parameters["expression"] = px::vn::ExpressionToValue(
        px::vn::Expression::Binary(px::vn::ExpressionOperator::GreaterEqual,
                                   px::vn::Expression::Variable("affection"),
                                   px::vn::Expression::Literal(3)));
    px::vn::scenario::ScenarioNode end{px::Uuid::FromName("condition-end"), "chapter", {{"title",std::string("End")}}};
    expressionScenario.entry = condition.id;
    expressionScenario.nodes = {condition, end};
    expressionScenario.edges.push_back({px::Uuid::FromName("condition-true"), condition.id,
                                        "true", end.id, "in"});
    expressionScenario.edges.push_back({px::Uuid::FromName("condition-false"), condition.id,
                                        "false", end.id, "in"});
    Check(px::vn::scenario::ValidateScenario(expressionScenario).Valid(),
          "typed condition should satisfy the strict shared command schema");
    const auto scenarioProgram = px::vn::scenario::CompileScenario(expressionScenario);
    const auto typedIf = std::find_if(scenarioProgram.code.begin(), scenarioProgram.code.end(),
                                      [](const px::vn::Command& command) {
                                          return command.type == "branch";
                                      });
    Check(scenarioProgram.errors.empty() && typedIf != scenarioProgram.code.end() &&
              typedIf->FindTyped("expression"),
          "Scenario IR should compile directly into VM code without a text projection round-trip");

    px::vn::VariableStore store;
    store.SetValue("route", px::vn::Value("alice"));
    store.SetValue("flags", px::vn::Value(px::vn::ValueList{true, "seen"}),
                   px::vn::VariableScope::Persistent);
    const auto fromStore = store.Evaluate(px::vn::Expression::Binary(
        px::vn::ExpressionOperator::Equal, px::vn::Expression::Variable("route"),
        px::vn::Expression::Literal("alice")));
    Check(fromStore && fromStore.Value().TryGet<bool>() && *fromStore.Value().TryGet<bool>() &&
              store.PersistentKeys().contains("flags"),
          "variable store should retain typed list/map/string values with explicit scope");
}

void TestRouteStackAndModalState() {
    px::ui::RouteTable table;
    Check(static_cast<bool>(table.Register(
              {"title", px::resource::ResourceRef<px::ui::UISceneResourceTag>(
                            px::Uuid::FromName("ui-title"), "Content/UI/Title.pxscene")})),
          "typed route table should accept a ResourceId-backed scene");
    Check(table.Find("title") && table.Find("title")->scene.LastKnownPath().ends_with("Title.pxscene"),
          "route table should resolve stable route ids");

    px::ui::UIRouter router;
    const auto factory = []() -> px::Result<std::unique_ptr<px::ui::Control>> {
        return px::Result<std::unique_ptr<px::ui::Control>>::Success(
            std::make_unique<px::ui::Control>());
    };
    Check(router.Register("title", factory) && router.Register("game", factory) &&
              router.Register("confirm", factory),
          "router factories should register");
    Check(router.Push("title") && router.Push("game") && router.ShowModal("confirm") &&
              router.ShowModal("confirm"),
          "router should support push and a true modal stack");
    const auto captured = router.CaptureState();
    Check(captured.stack.size() == 2 && captured.modals.size() == 2,
          "router state should capture route and modal stacks");
    Check(router.CloseModal() && router.CurrentModalRoute() == "confirm",
          "closing one modal should reveal the previous modal");
    Check(router.RestoreState(captured) && router.CurrentRoute() == "game" &&
              router.CurrentModalRoute() == "confirm",
          "router stack should restore atomically from runtime state");
}

void TestLuaExtensionSandbox() {
    TempDirectory temp;
    const auto extensionDirectory = temp.path / "Content" / "Extensions";
    std::filesystem::create_directories(extensionDirectory);
    {
        std::ofstream module(extensionDirectory / "helper.lua");
        module << "return { value = 42 }\n";
        std::ofstream entry(extensionDirectory / "demo.lua");
        entry << "local h=require('helper'); assert(h.value==42); "
                 "Engine.RegisterCommand('demo.command', function(args) return true end); "
                 "Engine.RegisterCommand('demo.await', function(args) Engine.AwaitSeconds(0.01); return true end)\n";
        std::ofstream manifest(extensionDirectory / "default.pxextension");
        manifest << R"({"format":"PrismatiXExtension","version":3,"id":"demo","entry":"demo.lua","capabilities":["runtime"],"commands":[{"id":"demo.command","displayName":"Demo","category":"Extension","await":false,"rollback":"reversible","parameters":[]},{"id":"demo.await","displayName":"Await","category":"Extension","await":true,"rollback":"boundary","parameters":[]}]})";
        std::ofstream denied(extensionDirectory / "denied.pxextension");
        denied << R"({"format":"PrismatiXExtension","version":3,"id":"denied","entry":"demo.lua","capabilities":["filesystem"],"commands":[]})";
    }
    px::io::VFS vfs;
    vfs.MountDirectory(temp.path.string());
    px::lua::LuaServices services;
    services.vfs = &vfs;
    px::lua::LuaHost host(services);
    Check(host.RunString("assert(os == nil and io == nil and debug == nil and dofile == nil and loadfile == nil)"),
          "Lua should not expose host filesystem, process, or debug libraries by default");
    Check(host.LoadExtensionManifest("Content/Extensions/default.pxextension"),
          "declared extension should load through VFS-aware require");
    px::vn::Command awaitCommand; awaitCommand.type="demo.await";
    Check(host.InvokeCommand(awaitCommand) && host.HasPendingCommand(),
          "Lua custom commands should suspend as coroutines instead of blocking the runtime");
    const auto luaCheckpoint = host.CapturePending();
    host.CancelPending();
    Check(host.RestorePending(luaCheckpoint) && host.HasPendingCommand(),
          "Lua commands should reconstruct the exact declared await boundary from a save");
    host.Update(0.02f);
    Check(!host.HasPendingCommand(), "Lua timer await should resume at a frame-safe checkpoint");
    Check(!host.LoadExtensionManifest("Content/Extensions/denied.pxextension"),
          "undeployed filesystem capability should be rejected explicitly");
}

void TestUnifiedTimelineAndPresets() {
    px::animation::AnimationClip clip;
    clip.id = px::Uuid::FromName("timeline-test");
    clip.name = "Test";
    clip.duration = 1.0f;
    clip.tracks.push_back({{px::animation::TargetKind::Camera, "main", "zoom"},
                           {{0.0f, 1.0, px::animation::Curve::Linear},
                            {1.0f, 2.0, px::animation::Curve::EaseInOut}}});
    clip.markers.push_back({0.5f, "half", {}});
    px::animation::TimelinePlayer player;
    double applied = 0.0;
    int markers = 0;
    int completed = 0;
    player.SetApply([&](const px::animation::TrackBinding&, const px::Variant& value) {
        if (const auto* number = value.TryGet<double>()) applied = *number;
        return px::Status::Ok();
    });
    player.SetEvent([&](const px::animation::Marker&) { ++markers; });
    player.SetCompletion([&](px::animation::PlaybackHandle, bool cancelled) {
        if (!cancelled) ++completed;
    });
    Check(static_cast<bool>(player.Register(clip)), "valid unified animation clip should register");
    const auto handle = player.Play(clip.id, true);
    player.Update(0.6f);
    Check(handle != 0 && applied > 1.0 && applied < 2.0 && markers == 1,
          "timeline should sample curves and emit markers while playing");
    const auto state = player.CaptureState();
    player.Update(0.5f);
    Check(!player.Playing(handle) && completed == 1,
          "awaitable timeline playback should complete without blocking the thread");
    Check(player.RestoreState(state) && player.Playing(handle),
          "timeline playback should restore its exact mid-animation state");
    const std::string encoded = px::animation::WriteAnimationClip(clip);
    auto decoded = px::animation::ParseAnimationClip(encoded, "memory.pxanim");
    Check(decoded && decoded.Value().id == clip.id && decoded.Value().tracks.size() == 1,
          ".pxanim version 3 should round-trip typed keyframes exactly");
    px::animation::AnimationClip child;
    child.id = px::Uuid::FromName("timeline-child"); child.name = "Child"; child.duration = 1.0f;
    child.tracks.push_back({{px::animation::TargetKind::UI, "panel", "alpha"},
                            {{0.0f, 0.0, px::animation::Curve::Linear},
                             {1.0f, 1.0, px::animation::Curve::Linear}}});
    px::animation::AnimationClip parent;
    parent.id = px::Uuid::FromName("timeline-parent"); parent.name = "Parent"; parent.duration = 2.0f;
    parent.nested.push_back({0.5f, child.id, 2.0f});
    Check(player.Register(child) && player.Register(parent),
          "nested clips should register as ordinary reusable animation assets");
    const auto nestedHandle = player.Play(parent.id);
    player.Update(0.75f);
    Check(nestedHandle != 0 && applied > 0.4 && applied < 0.6,
          "nested clips should be sampled with start offset and speed");
    Check(px::animation::OfficialPresets().size() >= 35,
          "official preset library should cover text, actor, screen, and UI effects");
}

void TestStoryMap() {
    px::vn::scenario::ScenarioDocument source;
    source.id = px::Uuid::FromName("story-source");
    source.name = "Source";
    px::vn::scenario::ScenarioNode jump;
    jump.id = px::Uuid::FromName("story-jump");
    jump.command = "jump";
    source.entry = jump.id;
    source.nodes.push_back(jump);
    const px::vn::scenario::StoryTarget target{
        px::Uuid::FromName("story-target"), px::Uuid::FromName("story-entry"),
        "Content/Scenario/target.pxscenario"};
    Check(source.nodes.front().parameters.empty() &&
              px::vn::scenario::ConnectStoryTarget(source, jump.id, "target", target),
          "Story Map connection should update the source Scenario explicitly");
    const auto links = px::vn::scenario::DeriveStoryLinks(source);
    const auto program = px::vn::scenario::CompileScenario(source);
    const auto runtimeJump = std::find_if(program.code.begin(), program.code.end(),
                                          [](const px::vn::Command& command) {
                                              return command.type == "jump";
                                          });
    Check(links.size() == 1 && links.front().target.scenario == target.scenario &&
              runtimeJump != program.code.end() &&
              runtimeJump->Get("target").starts_with("Content/Scenario/target.pxscenario#"),
          "Story Map should be derived from ResourceId targets and compile to an entry route");
    Check(px::vn::scenario::DisconnectStoryTarget(source, jump.id, "target") &&
              px::vn::scenario::DeriveStoryLinks(source).empty(),
          "deleting a Story Map link should clear the explicit source target");

}

px::Variant ContractValue(const px::vn::CommandParameterDescriptor& parameter){switch(parameter.type){case px::VariantType::Null:return px::Variant(1);case px::VariantType::Bool:return px::Variant(false);case px::VariantType::Integer:return px::Variant(std::int64_t{1});case px::VariantType::Number:return px::Variant(1.0);case px::VariantType::String:return px::Variant(parameter.name=="textId"?px::Uuid::Random().ToString():std::string("value"));case px::VariantType::ResourceRef:return px::ResourceRefValue{px::Uuid::Random(),"Content/test.asset"};case px::VariantType::Object:return parameter.widget==px::vn::CommandEditorWidget::Expression?px::vn::ExpressionToValue(px::vn::Expression::Literal(true)):px::VariantObject{};case px::VariantType::Array:return px::VariantArray{};case px::VariantType::Uuid:return px::Uuid::Random();default:return parameter.defaultValue.Clone();}}

void TestEveryCommandDescriptorContract(){for(const auto& descriptor:px::vn::CommandRegistry::Builtins().Descriptors()){px::vn::scenario::ScenarioDocument document;document.id=px::Uuid::Random();document.name=descriptor.id;px::vn::scenario::ScenarioNode node;node.id=px::Uuid::Random();node.command=descriptor.id;for(const auto& parameter:descriptor.parameters)if(parameter.required)node.parameters[parameter.name]=ContractValue(parameter);if(descriptor.id=="jump"||descriptor.id=="call")node.parameters["target"]="@"+node.id.ToString();document.entry=node.id;const auto nodeId=node.id;document.nodes.push_back(std::move(node));if(descriptor.id=="choice")document.edges.push_back({px::Uuid::Random(),nodeId,"choice",nodeId,"in"});if(descriptor.id=="branch"){document.edges.push_back({px::Uuid::Random(),nodeId,"true",nodeId,"in"});document.edges.push_back({px::Uuid::Random(),nodeId,"false",nodeId,"in"});}const auto encoded=px::vn::scenario::WriteScenario(document);const auto parsed=px::vn::scenario::ParseScenario(encoded,"contract.pxscenario");Check(parsed&&px::vn::scenario::ValidateScenario(parsed.Value()).Valid(),("command contract failed: "+descriptor.id).c_str());}}

void TestVisualGraphControlFlowContract(){
    using namespace px::vn::scenario;
    ScenarioDocument document;document.id=px::Uuid::Random();document.name="visual flow";
    ScenarioNode first{px::Uuid::Random(),"choice",{{"textId",std::string("choice-a")},{"text",std::string("A")}}};
    ScenarioNode second{px::Uuid::Random(),"choice",{{"textId",std::string("choice-b")},{"text",std::string("B")}}};
    ScenarioNode resultA{px::Uuid::Random(),"chapter",{{"title",std::string("A result")}}};
    ScenarioNode resultB{px::Uuid::Random(),"chapter",{{"title",std::string("B result")}}};
    document.entry=first.id;document.nodes={resultB,first,resultA,second};
    document.edges={{px::Uuid::Random(),first.id,"flow",second.id,"in"},
                    {px::Uuid::Random(),first.id,"choice",resultA.id,"in"},
                    {px::Uuid::Random(),second.id,"choice",resultB.id,"in"}};
    Check(ValidateScenario(document).Valid(),"linked visual Choice nodes should validate");
    const auto program=CompileScenario(document);std::vector<const px::vn::Command*> choices;
    for(const auto& command:program.code)if(command.type=="choice")choices.push_back(&command);
    Check(choices.size()==2&&choices[0]->Get("text")=="A"&&choices[1]->Get("text")=="B"&&
          !choices[0]->Get("target").empty()&&!choices[1]->Get("target").empty(),
          "visual Choice flow order and branch targets must compile independently of storage order");

    ScenarioDocument branchDocument;branchDocument.id=px::Uuid::Random();branchDocument.name="branch";
    ScenarioNode branch{px::Uuid::Random(),"branch",{{"expression",px::vn::ExpressionToValue(px::vn::Expression::Literal(true))}}};
    branchDocument.entry=branch.id;branchDocument.nodes={branch,resultA,resultB};
    branchDocument.edges={{px::Uuid::Random(),branch.id,"true",resultA.id,"in"},
                          {px::Uuid::Random(),branch.id,"false",resultB.id,"in"}};
    const auto branchProgram=CompileScenario(branchDocument);
    const auto runtimeBranch=std::find_if(branchProgram.code.begin(),branchProgram.code.end(),[](const auto& command){return command.type=="branch";});
    Check(ValidateScenario(branchDocument).Valid()&&runtimeBranch!=branchProgram.code.end()&&
          !runtimeBranch->Get("trueTarget").empty()&&!runtimeBranch->Get("falseTarget").empty(),
          "If True/False ports must compile to explicit runtime targets");
}

void TestLargeScenarioScale(){px::vn::scenario::ScenarioDocument document;document.id=px::Uuid::Random();document.name="10k acceptance";document.nodes.reserve(10000);document.edges.reserve(9999);for(int i=0;i<10000;++i){px::vn::scenario::ScenarioNode node{px::Uuid::Random(),"say",{{"textId",px::Uuid::Random().ToString()},{"value",std::string("line ")+std::to_string(i)}}};if(i==0)document.entry=node.id;if(i>0)document.edges.push_back({px::Uuid::Random(),document.nodes.back().id,"flow",node.id,"in"});document.nodes.push_back(std::move(node));}const auto encoded=px::vn::scenario::WriteScenario(document);const auto parsed=px::vn::scenario::ParseScenario(encoded,"large.pxscenario");Check(parsed&&parsed.Value().nodes.size()==10000&&px::vn::scenario::ValidateScenario(parsed.Value()).Valid(),"10,000-line Scenario must serialize, parse, and validate for creator acceptance");}

void TestCjkRubyAndVerticalText(){const auto rich=px::text::ParseRubyMarkup("[ruby=かんじ]漢字[/ruby][br]測試");Check(rich.plain=="漢字\n測試"&&rich.ruby.size()==1&&rich.ruby.front().reading=="かんじ","rich text should retain CJK ruby annotations");const auto wrapped=px::text::ApplyCjkKinsoku("這是一段測試，不能讓標點出現在行首。",6);Check(wrapped.find("\n，")==std::string::npos&&wrapped.find("\n。")==std::string::npos,"CJK wrapping should enforce kinsoku punctuation rules");const auto vertical=px::text::LayoutVertical("縱書ABC",4);Check(!vertical.empty()&&vertical.back().column>0&&vertical[2].rotate,"vertical layout should rotate Latin glyphs and advance columns");}

void TestAcceleratedEightHourSoak(){px::animation::AnimationClip clip;clip.id=px::Uuid::FromName("acceptance-soak");clip.name="Eight hour soak";clip.duration=2.0f;clip.loop=true;clip.tracks.push_back({{px::animation::TargetKind::Camera,"main","zoom"},{{0.0f,1.0,px::animation::Curve::Linear},{2.0f,1.1,px::animation::Curve::EaseInOut}}});px::animation::TimelinePlayer player;std::uint64_t samples=0;player.SetApply([&](const px::animation::TrackBinding&,const px::Variant&){++samples;return px::Status::Ok();});Check(player.Register(clip),"soak animation should register");const auto handle=player.Play(clip.id);constexpr int updates=8*60*60*4;for(int i=0;i<updates;++i)player.Update(0.25f);const auto state=player.CaptureState();Check(player.Playing(handle)&&state.size()==1&&state.front().loopIteration>=14399&&samples>=updates,"accelerated eight-hour timeline soak should remain bounded and playing");}

}  // namespace

int main() {
    TestArchiveBoundsAndRoundTrip();
    TestSaveValidation();
    TestTypedUIEventBinding();
    TestDialogueEffects();
    TestCommandSchemaAndScenarioRoundTrip();
    TestTypedExpressions();
    TestRouteStackAndModalState();
    TestLuaExtensionSandbox();
    TestUnifiedTimelineAndPresets();
    TestStoryMap();
    TestEveryCommandDescriptorContract();
    TestVisualGraphControlFlowContract();
    TestLargeScenarioScale();
    TestCjkRubyAndVerticalText();
    TestAcceleratedEightHourSoak();
    if (g_failures == 0) std::cout << "All PrismatiX commercial acceptance tests passed.\n";
    return g_failures == 0 ? 0 : 1;
}
