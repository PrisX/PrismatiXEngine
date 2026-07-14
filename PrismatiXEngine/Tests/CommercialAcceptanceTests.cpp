#include "Engine/IO/Archive.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/IO/VFS.h"
#include "Engine/Lua/LuaHost.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UISchemaMigration.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/UI/Widgets.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/Styles/StyleResolver.h"
#include "Engine/UI/Actions/ActionCatalog.h"
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
        stream.write("PDX4", 4);
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
    const px::Uuid behaviorEntry=px::Uuid::FromName("save.behavior.entry");
    const px::Uuid behaviorDelay=px::Uuid::FromName("save.behavior.delay");
    px::ui::BehaviorFiberState behaviorFiber;
    behaviorFiber.id=9;behaviorFiber.entry=behaviorEntry;behaviorFiber.current=behaviorDelay;
    behaviorFiber.continuation.push_back(behaviorEntry);behaviorFiber.delayRemaining=0.35f;
    behaviorFiber.actionExecution=41;behaviorFiber.signalArguments["position"]=px::Variant(px::Vec2{12.0f,24.0f});
    snapshot.behavior.fibers.push_back(std::move(behaviorFiber));
    px::ui::ActionInvocation savedActionInvocation;
    savedActionInvocation.action="demo.async";savedActionInvocation.arguments["message"]=px::Variant(std::string("checkpoint"));
    savedActionInvocation.context.sourceScene="Content/UI/HUD.pxscene";
    savedActionInvocation.context.sourceNode=behaviorDelay;savedActionInvocation.context.signal="behavior";
    snapshot.behavior.actions.push_back({41,savedActionInvocation,"lua",73,false});
    px::lua::PendingActionState pendingAction;pendingAction.id=73;pendingAction.invocation=savedActionInvocation;
    pendingAction.yieldIndex=2;pendingAction.waitKind="timer";pendingAction.remainingSeconds=0.35f;
    snapshot.luaActions.push_back(std::move(pendingAction));
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
              loaded->luaPending.size() == 1 && loaded->luaPending.front().yieldIndex == 1 &&
              loaded->luaActions.size() == 1 && loaded->luaActions.front().yieldIndex == 2 &&
              loaded->behavior.fibers.size() == 1 &&
              loaded->behavior.fibers.front().current == behaviorDelay &&
              loaded->behavior.fibers.front().signalArguments.at("position").TryGet<px::Vec2>() &&
              loaded->behavior.actions.size() == 1 &&
              loaded->behavior.actions.front().providerHandle == 73,
          "version 4 save should preserve exact VM, stage, audio, Lua Action, Behavior, and variable state");

    px::progress::Json wrongType{{"version", 2}, {"variables", {{"affection", "high"}}}};
    Check(px::progress::SaveJson(saves.SlotPath(1), wrongType, nullptr),
          "wrong-type fixture should be written");
    Check(!saves.Load(1), "wrong-type save must fail without throwing");

    px::progress::Json removedV2{{"version", 2}, {"scriptPath", "removed"}};
    Check(px::progress::SaveJson(saves.SlotPath(3), removedV2, nullptr),
          "removed v2 fixture should be written");
    Check(!saves.Load(3), "version 2 saves must be rejected after the strict v4 cutover");

    px::progress::Json wrongVersion{{"version", 999}};
    Check(px::progress::SaveJson(saves.SlotPath(2), wrongVersion, nullptr),
          "wrong-version fixture should be written");
    Check(!saves.Load(2), "unsupported save version must be rejected");
    Check(!saves.Peek(2).exists, "unsupported save must not appear as a valid slot");
}

void TestTypedUITriggerBinding() {
    px::resource::TypedDocument document;
    document.kind = px::resource::DocumentKind::Scene;
    document.id = px::Uuid::Random();
    document.type = "UIScene";
    document.properties["uiSchemaVersion"] = std::int64_t{5};
    px::resource::NodeRecord button;
    button.id = px::Uuid::Random();
    button.type = "Button";
    button.name = "Start";
    button.properties["triggers"] = px::VariantObject{{"activated",px::VariantObject{
        {"kind",std::string("action")},{"action",std::string("game.start")},
        {"arguments",px::VariantObject{}},{"reentry",std::string("Allow")}}}};
    document.nodes.push_back(std::move(button));

    px::ui::FormatterRegistry formatters;
    const auto loaded = px::ui::InstantiateUIScene(document, nullptr, formatters);
    Check(static_cast<bool>(loaded), "typed TriggerBinding scene should load");
    if (loaded) {
        Check(loaded.Value().triggers.size()==1&&loaded.Value().triggers.front().signal=="activated"&&
                  loaded.Value().triggers.front().action=="game.start",
              "typed TriggerBinding should produce one generic runtime signal handler");
    }
    const auto directTrigger=[](const std::string& action){return px::VariantObject{{"activated",px::VariantObject{{"kind",std::string("action")},{"action",action},{"arguments",px::VariantObject{}},{"reentry",std::string("Allow")}}}};};
    document.nodes.front().properties["triggers"]=directTrigger("missing.action");
    Check(!px::ui::InstantiateUIScene(document,nullptr,formatters),"UI schema v5 must reject Direct Action Trigger bindings with missing Action ids");
    document.nodes.front().properties["triggers"]=px::VariantObject{{"missingSignal",px::VariantObject{{"kind",std::string("action")},{"action",std::string("game.start")},{"arguments",px::VariantObject{}},{"reentry",std::string("Allow")}}}};
    Check(!px::ui::InstantiateUIScene(document,nullptr,formatters),"UI schema v5 must reject Trigger bindings to invalid Control Signals");
    px::ui::BehaviorGraph emptyInteraction;document.properties["interactionGraph"]=px::ui::WriteBehaviorGraph(emptyInteraction);document.nodes.front().properties["triggers"]=px::VariantObject{{"activated",px::VariantObject{{"kind",std::string("flow")},{"entry",px::Uuid::Random()},{"reentry",std::string("Allow")}}}};
    Check(!px::ui::InstantiateUIScene(document,nullptr,formatters),"UI schema v5 must reject Flow Trigger bindings with missing Entry references");
    document.properties.erase("interactionGraph");document.nodes.front().properties["triggers"]=directTrigger("game.start");
    auto duplicate=document.nodes.front();duplicate.name="Duplicate";document.nodes.push_back(std::move(duplicate));
    Check(!px::ui::InstantiateUIScene(document,nullptr,formatters),"UI schema v5 must reject duplicate node UUIDs");document.nodes.pop_back();
    document.properties.erase("uiSchemaVersion");
    Check(!px::ui::InstantiateUIScene(document,nullptr,formatters),
          "UI loader must reject documents without semantic schema 5");
    document.properties["uiSchemaVersion"]=std::int64_t{5};
    document.nodes.front().properties["visible"]=true;
    Check(!px::ui::InstantiateUIScene(document,nullptr,formatters),
          "UI loader must reject removed legacy properties");
}

void TestDialogueEffects() {
    px::vn::Dialogue dialogue;
    dialogue.SetText("A", "Hello", 0, {}, {}, {}, "shake");
    dialogue.Update(100);
    dialogue.Update(350);
    Check(dialogue.State().effect == "shake" && dialogue.State().effectProgress >= 0.24f,
          "dialogue effect should remain animated after typewriter completion");

    px::ui::GalgameUI hud;px::ui::DialoguePresentation presentation;presentation.text="First line";
    Check(hud.ShowHUD(presentation),"HUD should be created for dialogue input regression test");
    px::Input input;input.InjectFrame(-1000,-1000,false);(void)hud.Update(input,1280,720);
    input.InjectFrame(640,600,true);
    Check(!hud.Update(input,1280,720),"non-interactive HUD panels must not consume dialogue advance clicks");

}

void TestSavedAssetIdentityRegistration(){TempDirectory temp;std::filesystem::create_directories(temp.path/"Content/UI");std::filesystem::create_directories(temp.path/"Content/Scenario");const auto scene=temp.path/"Content/UI/test.pxscene";const auto scenario=temp.path/"Content/Scenario/start.pxscenario";const auto layout=temp.path/"Content/Scenario/start.pxlayout";std::ofstream(scene)<<"scene";std::ofstream(scenario)<<"scenario";std::ofstream(layout)<<"layout";px::resource::AssetRegistry registry;const auto registered=registry.RegisterAsset(temp.path,scene,"scene");const auto registeredScenario=registry.RegisterAsset(temp.path,scenario,"script");const auto registeredLayout=registry.RegisterAsset(temp.path,layout,"other");Check(static_cast<bool>(registered),"saved scene should receive identity metadata");Check(static_cast<bool>(registeredScenario)&&static_cast<bool>(registeredLayout),"saved Scenario and its layout companion should both receive identity metadata");Check(std::filesystem::exists(px::resource::AssetRegistry::MetaPath(scene)),"scene registration should create a .pxmeta file");Check(std::filesystem::exists(px::resource::AssetRegistry::MetaPath(scenario))&&std::filesystem::exists(px::resource::AssetRegistry::MetaPath(layout)),"Scenario save should create identity metadata for both output files");Check(registry.Scan(temp.path),"asset scan should accept all registered saved artifacts");}

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
                 "Engine.RegisterCommand('demo.await', function(args) Engine.AwaitSeconds(0.01); return true end); "
                 "Engine.RegisterAction('demo.typed', function(args, context) assert(args.mode == 'alpha' and args.amount == 2.0); Engine.AwaitSeconds(0.01); return true end)\n";
        std::ofstream manifest(extensionDirectory / "default.pxextension");
        manifest << R"({"format":"PrismatiXExtension","version":4,"id":"demo","entry":"demo.lua","capabilities":["runtime","ui"],"commands":[{"id":"demo.command","displayName":"Demo","category":"Extension","await":false,"rollback":"reversible","parameters":[]},{"id":"demo.await","displayName":"Await","category":"Extension","await":true,"rollback":"boundary","parameters":[]}],"actions":[{"id":"demo.typed","displayName":"Typed Action","description":"Manifest metadata test","category":"Extension","reentry":"Restart","capabilities":["runtime","ui"],"parameters":[{"name":"mode","displayName":"Mode","description":"Typed enum","type":"string","required":true,"default":"alpha","enum":["alpha","beta"],"editorHint":"enum"},{"name":"amount","displayName":"Amount","type":"number","default":1.5,"range":{"minimum":0.0,"maximum":10.0}},{"name":"asset","displayName":"Asset","type":"resource","default":"Content/Images/test.png","resourceFilter":"image","editorHint":"resource"},{"name":"tint","displayName":"Tint","type":"color","default":[255,128,64,255],"editorHint":"color"}]}]})";
        std::ofstream denied(extensionDirectory / "denied.pxextension");
        denied << R"({"format":"PrismatiXExtension","version":4,"id":"denied","entry":"demo.lua","capabilities":["filesystem"],"commands":[]})";
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
    const auto* typedAction=px::ui::ActionCatalog::Global().Find("demo.typed");
    Check(typedAction&&typedAction->reentryPolicy==px::ui::ActionReentryPolicy::Restart&&typedAction->capabilities.size()==2&&typedAction->arguments.size()==4&&typedAction->arguments[0].enumValues.size()==2&&typedAction->arguments[1].minimum&&typedAction->arguments[1].maximum&&typedAction->arguments[2].resourceType=="image"&&typedAction->arguments[3].defaultValue&&typedAction->arguments[3].defaultValue->TryGet<px::Color>(),
          "Lua Action manifest should preserve capabilities, defaults, enum, range, resource filter, editor hint, and reentry metadata");
    px::ui::ActionInvocation actionInvocation;actionInvocation.action="demo.typed";actionInvocation.arguments={{"mode",std::string("alpha")},{"amount",2.0}};
    const auto actionStart=host.StartAction(actionInvocation);Check(actionStart.status&&actionStart.pending,"Lua Action should run as a tracked coroutine");const auto actionCheckpoint=host.CapturePendingActions();
    Check(host.RestorePending({})&&host.RestorePendingActions(actionCheckpoint)&&host.ActionState(actionStart.handle)==px::ui::ActionExecutionState::Running,"Lua Action should reconstruct its exact yield checkpoint");host.Update(0.02f);Check(host.ActionState(actionStart.handle)==px::ui::ActionExecutionState::Completed,"restored Lua Action should resume to completion");
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
          ".pxanim version 4 should round-trip typed keyframes exactly");
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

void TestTypedFormatV4TokensAndComponents() {
    Check(px::ui::RegisterBuiltinUITypes(),"built-in UI metadata should be available for component interface validation");
    px::resource::TypedDocument theme;
    theme.kind = px::resource::DocumentKind::Resource;
    theme.id = px::Uuid::Random();
    theme.type = "UITheme";
    px::ui::StyleThemeData styleData;
    const px::Uuid baseId=px::Uuid::FromName("color.base"),surfaceId=px::Uuid::FromName("color.surface");
    Check(styleData.UpsertToken({.id=baseId,.displayName="color.base",.type=px::VariantType::Color,.value=px::ui::StyleValue::Literal(px::Color{10,20,30,255})}),"base style token should register");
    Check(styleData.UpsertToken({.id=surfaceId,.displayName="color.surface",.type=px::VariantType::Color,.value=px::ui::StyleValue::Token(baseId,"color.base")}),"style token alias should register");
    theme.properties["styleSystem"] = px::ui::WriteStyleTheme(styleData);
    const std::string encoded = px::resource::WriteTypedDocument(theme);
    const auto parsed = px::resource::ParseTypedDocument(encoded, "theme.pxtheme");
    Check(parsed && encoded.starts_with("@pxresource 4 "),
          "typed resources must write and parse strict v4 headers");
    if (parsed) {
        const auto decodedStyle=px::ui::ParseStyleTheme(parsed.Value().properties.at("styleSystem"));
        const auto* alias=decodedStyle?decodedStyle.Value().FindToken(surfaceId):nullptr;
        const auto loadedTheme = px::ui::LoadUITheme(parsed.Value());
        const auto* resolved = loadedTheme ? loadedTheme.Value().FindToken("color.surface") : nullptr;
        Check(alias && alias->value.IsTokenReference()&&alias->value.TokenReference()==baseId&&resolved &&
                  resolved->TryGet<px::Color>() &&
                  *resolved->TryGet<px::Color>() == px::Color{10, 20, 30, 255},
              "token() must round-trip and resolve through typed theme aliases");
    }
    std::string legacy = encoded;
    const auto version = legacy.find(" 4 ");
    if (version != std::string::npos) legacy.replace(version, 3, " 3 ");
    Check(!px::resource::ParseTypedDocument(legacy, "legacy-v3.pxtheme"),
          "typed v3 resources must be rejected without a compatibility parser");

    px::resource::TypedDocument component;
    component.kind = px::resource::DocumentKind::Scene;
    component.id = px::Uuid::Random();
    component.type = "UIComponent";
    component.properties["uiSchemaVersion"]=std::int64_t{5};
    px::resource::NodeRecord root;
    root.id = px::Uuid::Random(); root.type = "Panel"; root.name = "Card";
    px::resource::NodeRecord label;
    label.id = px::Uuid::Random(); label.parent = root.id; label.type = "Label";
    label.name = "Caption"; label.properties["text"] = std::string("Default");
    component.nodes = {root, label};
    component.properties["component.exposedProperties"]=px::VariantArray{px::Variant(px::VariantObject{{"id",std::string("captionOpacity")},{"displayName",std::string("Caption Opacity")},{"node",label.id},{"property",std::string("opacity")},{"type",std::string("Number")}})};
    component.properties["component.exposedSignals"]=px::VariantArray{px::Variant(px::VariantObject{{"id",std::string("captionClicked")},{"displayName",std::string("Caption Clicked")},{"node",label.id},{"signal",std::string("clicked")}})};
    component.properties["component.slots"]=px::VariantArray{px::Variant(px::VariantObject{{"id",std::string("content")},{"displayName",std::string("Content")},{"node",label.id}})};

    px::resource::TypedDocument scene;
    scene.kind = px::resource::DocumentKind::Scene;
    scene.id = px::Uuid::Random(); scene.type = "UIScene";
    px::resource::NodeRecord instance;
    instance.id = px::Uuid::Random(); instance.type = "ComponentInstance"; instance.name = "Card 1";
    instance.properties["component"] = px::ResourceRefValue{component.id, "Content/UI/Card.pxcomponent"};
    instance.properties["overrides"] = px::VariantObject{
        {label.id.ToString(), px::VariantObject{{"text", std::string("Overridden")}}},
    };
    instance.properties["componentProperties"]=px::VariantObject{{"captionOpacity",0.4}};
    instance.properties["componentEvents"]=px::VariantObject{{"captionClicked",px::VariantObject{{"kind",std::string("action")},{"action",std::string("game.start")},{"arguments",px::VariantObject{}},{"reentry",std::string("Allow")}}}};
    px::resource::NodeRecord slotted;slotted.id=px::Uuid::Random();slotted.parent=instance.id;slotted.type="Button";slotted.name="Projected";slotted.properties["componentSlot"]=std::string("content");
    scene.nodes={instance,slotted};
    const px::ui::UIDocumentLoader loader = [&component](const px::ResourceRefValue& reference) {
        (void)reference;
        return px::Result<px::resource::TypedDocument>::Success(component);
    };
    const auto first = px::ui::ExpandUIComponents(scene, loader);
    const auto second = px::ui::ExpandUIComponents(scene, loader);
    bool stable = first && second && first.Value().document.nodes.size() == 3 &&
                  second.Value().document.nodes.size() == 3;
    if (stable) {
        const auto& expandedRoot = first.Value().document.nodes[0];
        const auto& expandedLabel = first.Value().document.nodes[1];
        const auto& expandedSlotChild = first.Value().document.nodes[2];
        const auto* opacity=expandedLabel.properties.contains("opacity")?expandedLabel.properties.at("opacity").TryGet<double>():nullptr;
        const auto* triggers=expandedLabel.properties.contains("triggers")?expandedLabel.properties.at("triggers").AsObject():nullptr;
        stable = expandedRoot.id == instance.id && expandedLabel.id == second.Value().document.nodes[1].id &&
                 expandedLabel.properties.at("text").TryGet<std::string>() &&
                 *expandedLabel.properties.at("text").TryGet<std::string>() == "Overridden"&&opacity&&std::abs(*opacity-.4)<.001&&triggers&&triggers->contains("clicked")&&expandedSlotChild.parent==expandedLabel.id&&!expandedSlotChild.properties.contains("componentSlot");
    }
    Check(stable, "component expansion must apply overrides, exposed properties/signals/slots, and stable instance UUIDs");

    scene.nodes.front().properties["componentProperties"]=px::VariantObject{{"notExposed",true}};
    Check(!px::ui::ExpandUIComponents(scene,loader),"component instances must reject values for properties that are not exposed");
    scene.nodes.front()=instance;

    component.nodes.push_back(instance);
    component.nodes.back().parent = root.id;
    const auto cyclic = px::ui::ExpandUIComponents(scene, loader);
    Check(!cyclic, "nested component dependency cycles must be rejected");
}

void TestDesignerImageAndTextProperties(){
    px::resource::TypedDocument scene;scene.kind=px::resource::DocumentKind::Scene;scene.id=px::Uuid::Random();scene.type="UIScene";
    scene.properties["uiSchemaVersion"]=std::int64_t{5};
    px::resource::NodeRecord root;root.id=px::Uuid::Random();root.type="Panel";root.name="Root";
    px::resource::NodeRecord image;image.id=px::Uuid::Random();image.parent=root.id;image.type="TextureRect";image.name="Background";image.properties={{"path",std::string("Content/bg.png")},{"scaleMode",std::string("Fill")},{"lockAspectRatio",true},{"editorLocked",true}};
    px::resource::NodeRecord label;label.id=px::Uuid::Random();label.parent=root.id;label.type="Label";label.name="Centered";label.properties={{"text",std::string("Hello")},{"horizontalAlignment",std::string("Center")},{"verticalAlignment",std::string("Bottom")}};
    px::resource::NodeRecord button;button.id=px::Uuid::Random();button.parent=root.id;button.type="Button";button.name="Action";button.properties={{"text",std::string("Go")},{"horizontalAlignment",std::string("Right")},{"verticalAlignment",std::string("Top")}};scene.nodes={root,image,label,button};
    const auto loaded=px::ui::InstantiateUIScene(scene,nullptr,px::ui::FormatterRegistry{});bool valid=static_cast<bool>(loaded);
    if(valid){const auto* texture=dynamic_cast<const px::ui::TextureRect*>(loaded.Value().root->Find(image.id));const auto* text=dynamic_cast<const px::ui::Label*>(loaded.Value().root->Find(label.id));const auto* action=dynamic_cast<const px::ui::Button*>(loaded.Value().root->Find(button.id));valid=texture&&texture->ScaleMode()==px::ui::TextureScaleMode::Fill&&texture->LockAspectRatio()&&text&&text->HorizontalAlignment()==px::ui::HorizontalTextAlignment::Center&&text->VerticalAlignment()==px::ui::VerticalTextAlignment::Bottom&&action&&action->HorizontalAlignment()==px::ui::HorizontalTextAlignment::Right&&action->VerticalAlignment()==px::ui::VerticalTextAlignment::Top;}
    Check(valid,"designer image modes, editor-only lock metadata, and text alignment must load through typed UI scenes");
}

void TestAcceleratedEightHourSoak(){px::animation::AnimationClip clip;clip.id=px::Uuid::FromName("acceptance-soak");clip.name="Eight hour soak";clip.duration=2.0f;clip.loop=true;clip.tracks.push_back({{px::animation::TargetKind::Camera,"main","zoom"},{{0.0f,1.0,px::animation::Curve::Linear},{2.0f,1.1,px::animation::Curve::EaseInOut}}});px::animation::TimelinePlayer player;std::uint64_t samples=0;player.SetApply([&](const px::animation::TrackBinding&,const px::Variant&){++samples;return px::Status::Ok();});Check(player.Register(clip),"soak animation should register");const auto handle=player.Play(clip.id);constexpr int updates=8*60*60*4;for(int i=0;i<updates;++i)player.Update(0.25f);const auto state=player.CaptureState();Check(player.Playing(handle)&&state.size()==1&&state.front().loopIteration>=14399&&samples>=updates,"accelerated eight-hour timeline soak should remain bounded and playing");}

void TestDesignerRewriteContracts(){
    px::ui::StyleThemeData theme;px::ui::TokenDefinition token{.id=px::Uuid::FromName("accent"),.displayName="Accent",.type=px::VariantType::Color,.value=px::ui::StyleValue::Literal(px::Color{10,20,30,255})};Check(theme.UpsertToken(token),"style token should register");
    px::ui::StyleDefinition style;style.id=px::Uuid::FromName("primary-style");style.displayName="Primary";style.properties["background.color"]=px::ui::StyleValue::Token(token.id,"Accent");Check(theme.UpsertStyle(style),"style definition should register");
    px::ui::ControlStyleBinding binding;binding.baseStyle=style.id;const auto encoded=px::ui::WriteStyleTheme(theme);const auto decoded=px::ui::ParseStyleTheme(encoded);Check(decoded&&decoded.Value().FindToken(token.id)&&decoded.Value().FindStyle(style.id),"Style System 3 IDs and definitions must round-trip");
    px::ui::StyleResolveRequest request{.controlType="Button",.binding=binding,.activeStates=px::ui::StyleStateSet(px::ui::StyleState::Hover)};px::ui::StylePropertyRegistry properties;px::ui::StyleResolver resolver;const auto resolved=resolver.Resolve(theme,request,properties);Check(resolved&&resolved.Value().Find("background.color")&&resolved.Value().Find("background.color")->tokenChain.size()==1,"style resolver must preserve token source trace");
    px::ui::ActionInvocation action{.action="choice.select",.arguments={{"index",std::int64_t{2}}}};Check(static_cast<bool>(px::ui::ActionCatalog::Global().ValidateAndNormalize(action)),"typed action arguments must validate");action.arguments["index"]=std::string("bad");Check(!px::ui::ActionCatalog::Global().ValidateAndNormalize(action),"typed action arguments must reject mismatched types");
    px::resource::TypedDocument scene;scene.kind=px::resource::DocumentKind::Scene;scene.type="UIScene";scene.properties["uiSchemaVersion"]=std::int64_t{4};px::resource::NodeRecord button;button.id=px::Uuid::Random();button.type="Button";scene.nodes.push_back(button);Check(!px::ui::InstantiateUIScene(scene,nullptr,px::ui::FormatterRegistry{}),"strict UI schema must reject v4 without a compatibility branch");scene.properties["uiSchemaVersion"]=std::int64_t{5};scene.nodes[0].properties["command"]=std::string("game.start");Check(!px::ui::InstantiateUIScene(scene,nullptr,px::ui::FormatterRegistry{}),"strict UI schema must reject command instead of migrating it");scene.nodes[0].properties.erase("command");scene.nodes[0].properties["themeVariant"]=std::string("Button");Check(!px::ui::InstantiateUIScene(scene,nullptr,px::ui::FormatterRegistry{}),"strict UI schema must reject themeVariant instead of migrating it");
}

class CheckpointActionProvider final : public px::ui::IActionProvider {
public:
    [[nodiscard]] std::string_view ProviderId()const override{return "checkpoint-test";}
    [[nodiscard]] px::ui::ActionOrigin Origin()const override{return px::ui::ActionOrigin::BuiltIn;}
    [[nodiscard]] bool CanInvoke(std::string_view action)const override{return action=="test.behavior.async";}
    px::Status Invoke(const px::ui::ActionInvocation&)override{return px::Status::Ok();}
    px::ui::ProviderActionStart Start(const px::ui::ActionInvocation&)override{const auto id=m_next++;m_states[id]=px::ui::ActionExecutionState::Running;return{px::Status::Ok(),id,true};}
    [[nodiscard]] px::ui::ActionExecutionState Poll(const std::uint64_t handle)const override{const auto found=m_states.find(handle);return found==m_states.end()?px::ui::ActionExecutionState::Unknown:found->second;}
    void Cancel(const std::uint64_t handle)override{m_states[handle]=px::ui::ActionExecutionState::Cancelled;}
    void Update(float)override{for(auto& [_,state]:m_states)if(state==px::ui::ActionExecutionState::Running)state=px::ui::ActionExecutionState::Completed;}
private:std::unordered_map<std::uint64_t,px::ui::ActionExecutionState> m_states;std::uint64_t m_next=1;
};

void TestBehaviorGraphExecutionAndCheckpoint(){
    const auto entry=px::Uuid::FromName("behavior.test.entry"),action=px::Uuid::FromName("behavior.test.action"),constant=px::Uuid::FromName("behavior.test.constant"),set=px::Uuid::FromName("behavior.test.set");
    px::ui::BehaviorGraph graph;graph.nodes={
        {.id=entry,.kind=px::ui::BehaviorNodeKind::SignalEntry},
        {.id=action,.kind=px::ui::BehaviorNodeKind::Action,.properties={{"action",std::string("test.behavior.async")},{"wait",true}}},
        {.id=constant,.kind=px::ui::BehaviorNodeKind::Constant,.properties={{"value",std::int64_t{7}}}},
        {.id=set,.kind=px::ui::BehaviorNodeKind::SetVariable,.properties={{"name",std::string("result")}}}};
    graph.links={{px::Uuid::FromName("behavior.link.1"),entry,"out",action,"in"},
                 {px::Uuid::FromName("behavior.link.2"),action,"out",set,"in"},
                 {px::Uuid::FromName("behavior.link.3"),constant,"value",set,"value"}};
    Check(graph.Validate("behavior-test"),"valid typed Behavior Graph should pass validation");
    px::ui::ActionCatalog catalog;px::ui::ActionDescriptor descriptor;descriptor.id="test.behavior.async";descriptor.label="Async";descriptor.displayName="Async";descriptor.category="Test";descriptor.origin=px::ui::ActionOrigin::BuiltIn;descriptor.providerId="checkpoint-test";
    Check(catalog.Register(std::move(descriptor)),"Behavior test Action should register");
    auto provider=std::make_shared<CheckpointActionProvider>();px::ui::ActionDispatcher firstActions(catalog);Check(firstActions.RegisterProvider(provider),"Behavior test provider should register");
    std::int64_t result=0;px::ui::BehaviorRuntimeServices services;services.actions=&firstActions;services.writeVariable=[&](std::string_view,const px::Variant& value){const auto* integer=value.TryGet<std::int64_t>();if(integer)result=*integer;return px::Status::Ok();};
    px::ui::BehaviorGraphRunner first(services);Check(first.SetGraph(graph,"behavior-test"),"Behavior runner should accept valid graph");const auto started=first.Start(entry);
    Check(started&&first.ActiveFibers().size()==1&&first.ActiveFibers().front().actionExecution!=0,"awaited Action should suspend a Behavior fiber");
    const auto actionCheckpoint=firstActions.CaptureState();const auto fiberCheckpoint=first.CaptureState();
    px::ui::ActionDispatcher restoredActions(catalog);Check(restoredActions.RegisterProvider(provider),"restored dispatcher should register provider");Check(restoredActions.RestoreState(actionCheckpoint),"Action execution handles should restore before Behavior fibers");
    services.actions=&restoredActions;px::ui::BehaviorGraphRunner restored(services);Check(restored.SetGraph(graph,"behavior-test"),"restored runner should load the same graph");Check(restored.RestoreState(fiberCheckpoint),"Behavior fiber checkpoint should restore");
    restoredActions.Update(0.1f);restored.Update(0.1f);Check(result==7&&restored.ActiveFibers().empty(),"restored async Action should resume the fiber and preserve typed data flow");

    px::ui::BehaviorGraph typedError;const auto branch=px::Uuid::FromName("behavior.bad.branch"),text=px::Uuid::FromName("behavior.bad.text");typedError.nodes={{.id=branch,.kind=px::ui::BehaviorNodeKind::Branch},{.id=text,.kind=px::ui::BehaviorNodeKind::Constant,.properties={{"value",std::string("not bool")}}}};typedError.links={{px::Uuid::Random(),text,"value",branch,"condition"}};Check(!typedError.Validate(),"Behavior Graph should reject incompatible typed pins");
    px::ui::BehaviorGraph cycle;const auto a=px::Uuid::FromName("behavior.cycle.a"),b=px::Uuid::FromName("behavior.cycle.b");cycle.nodes={{.id=a,.kind=px::ui::BehaviorNodeKind::Sequence},{.id=b,.kind=px::ui::BehaviorNodeKind::Sequence}};cycle.links={{px::Uuid::Random(),a,"0",b,"in"},{px::Uuid::Random(),b,"0",a,"in"}};Check(!cycle.Validate(),"Behavior Graph should reject flow cycles at build time");

    px::ui::UIContext context;Check(context.SetRoot(std::make_unique<px::ui::Control>("AnimationRoot")),"UI context should accept an animation test root");bool externalRunning=true;std::string externalPath;context.SetExternalAnimationServices([&](const std::string_view path){externalPath=path;return px::Result<std::uint64_t>::Success(42);},[&](const std::uint64_t handle){return handle==42&&externalRunning;});const auto animationEntry=px::Uuid::Random(),playAnimation=px::Uuid::Random();px::ui::BehaviorGraph animationGraph;animationGraph.nodes={{.id=animationEntry,.kind=px::ui::BehaviorNodeKind::SignalEntry},{.id=playAnimation,.kind=px::ui::BehaviorNodeKind::PlayAnimation,.properties={{"name",std::string("Content/Animations/HUD.pxanim")},{"wait",true}}}};animationGraph.links={{px::Uuid::Random(),animationEntry,"out",playAnimation,"in"}};Check(context.ConfigureTriggers({},animationGraph,"animation-behavior"),"UI context should configure external animation Behavior nodes");const auto externalStarted=context.Behaviors().Start(animationEntry);Check(externalStarted&&externalPath=="Content/Animations/HUD.pxanim"&&context.Behaviors().ActiveFibers().size()==1,"Play Animation must route external .pxanim paths through the shared runtime timeline");context.Behaviors().Update(.1f);Check(context.Behaviors().ActiveFibers().size()==1,"waiting Behavior fibers must remain suspended while an external animation is running");externalRunning=false;context.Behaviors().Update(.1f);Check(context.Behaviors().ActiveFibers().empty(),"waiting Behavior fibers must resume when external animation playback completes");
    const auto localEntry=px::Uuid::Random(),localPlay=px::Uuid::Random(),localClipId=px::Uuid::Random(),localState=px::Uuid::Random();px::ui::AnimationClip localClip;localClip.id=localClipId;localClip.name="Default";localClip.duration=.1f;localClip.tracks.push_back({.node=context.Root()->Id(),.property="opacity",.keys={{0.0f,1.0,px::ui::Ease::Linear,px::ui::KeyInterpolation::Linear},{.1f,0.0,px::ui::Ease::Linear,px::ui::KeyInterpolation::Linear}}});px::ui::UIAnimationLibrary localLibrary;localLibrary.clips.push_back(std::move(localClip));localLibrary.machine.entry=localState;localLibrary.machine.states.push_back({localState,"Default",localClipId,{0,0}});Check(context.SetAnimations(std::move(localLibrary),false),"UI context should install the same local Animation Controller used by Player and Preview");px::ui::BehaviorGraph localGraph;localGraph.nodes={{.id=localEntry,.kind=px::ui::BehaviorNodeKind::SignalEntry},{.id=localPlay,.kind=px::ui::BehaviorNodeKind::PlayAnimation,.properties={{"name",std::string("Default")},{"wait",true}}}};localGraph.links={{px::Uuid::Random(),localEntry,"out",localPlay,"in"}};Check(context.ConfigureTriggers({},localGraph,"local-animation-behavior")&&context.Behaviors().Start(localEntry),"Behavior Flow should play a local State through the shared Animation Controller");px::Input emptyInput;(void)context.Update(emptyInput,100,100,.05f);Check(context.Behaviors().ActiveFibers().size()==1,"waiting Behavior Flow must remain suspended while a local Clip is active");(void)context.Update(emptyInput,100,100,.06f);Check(context.Behaviors().ActiveFibers().empty(),"waiting Behavior Flow must resume when the local Clip completes");
}

void TestExpandedControlMetadataAndTransforms(){
    Check(px::ui::RegisterBuiltinUITypes(),"built-in UI types should register");const auto& registry=px::TypeRegistry::Global();
    for(const char* name:{"NinePatchRect","TextEdit","OptionButton","SpinBox","RadioButton","Separator","ScrollBar","VideoRect"}){const auto* type=registry.Find(name);Check(type&&type->designer&&type->designer->paletteVisible,"new built-in controls must expose palette metadata");Check(static_cast<bool>(registry.Create(name)),"new built-in controls must be constructible from metadata");}
    Check(registry.FindSignal("OptionButton","itemSelected")&&registry.FindSignal("SpinBox","valueChanged")&&registry.FindSignal("TextEdit","textChanged"),"new control signals must expose named typed metadata");
    px::ui::Control transformed;transformed.Arrange({10,10,20,10});transformed.SetPivot({.5f,.5f});transformed.SetScale({2,2});transformed.SetRotation(90);
    Check(transformed.HitTest({20,15})&&transformed.HitTest({20,30})&&!transformed.HitTest({50,50}),"Control hit testing must match pivot, scale, and rotation");
    transformed.SetVisibility(px::ui::Visibility::Hidden);Check(!transformed.HitTest({20,15}),"Hidden controls must not participate in hit testing");
    px::ui::Control visibility;visibility.SetCustomMinimumSize({40,20});visibility.SetVisibility(px::ui::Visibility::Hidden);const auto hiddenSize=visibility.Measure({100,100});Check(hiddenSize.x==40&&hiddenSize.y==20,"Hidden controls must preserve their layout size");visibility.SetVisibility(px::ui::Visibility::Collapsed);const auto collapsedSize=visibility.Measure({100,100});Check(collapsedSize.x==0&&collapsedSize.y==0,"Collapsed controls must be removed from layout");
    px::ui::VideoRect video("Content/Video/intro.webm");bool decoderPlaying=false;int opens=0,updates=0,stops=0;(void)video.ConnectSignal("playbackStopped",[&](const auto&){++stops;});video.SetPlayback({[&](std::string_view path){++opens;decoderPlaying=path=="Content/Video/intro.webm";return decoderPlaying;},[&]{decoderPlaying=false;},[&](float){++updates;decoderPlaying=false;},[&]{return decoderPlaying;},{}, {}});Check(video.Playing()&&opens==1,"VideoRect autoplay must open its typed video resource through the host decoder");video.Update(.1f);Check(!video.Playing()&&updates==1&&stops==1,"VideoRect must publish playback completion instead of remaining a placeholder");video.SetLoop(true);video.SetPlaying(true);video.Update(.1f);Check(video.Playing()&&opens==3,"looping VideoRect playback must reopen the same decoder resource");
}

void TestEmbeddedUIAnimationParity(){
    Check(px::ui::RegisterBuiltinUITypes(),"built-in UI types should register for animation playback");
    px::resource::TypedDocument scene;scene.kind=px::resource::DocumentKind::Scene;scene.id=px::Uuid::Random();scene.type="UIScene";scene.properties["uiSchemaVersion"]=std::int64_t{5};
    px::resource::NodeRecord root;root.id=px::Uuid::Random();root.type="Control";root.name="Animated";scene.nodes.push_back(root);
    px::ui::AnimationClip clip;clip.id=px::Uuid::Random();clip.name="Default";clip.duration=1.0f;clip.tracks.push_back({.node=root.id,.property="opacity",.keys={{0.0f,1.0,px::ui::Ease::Linear,px::ui::KeyInterpolation::Linear},{1.0f,0.0,px::ui::Ease::Linear,px::ui::KeyInterpolation::Linear}}});
    px::ui::UIAnimationLibrary library;const px::Uuid clipId=clip.id,stateId=px::Uuid::Random();library.clips.push_back(std::move(clip));library.machine.entry=stateId;library.machine.states.push_back({stateId,"Default",clipId,{80,80}});scene.properties["animations"]=px::ui::WriteUIAnimationLibrary(library);
    const auto loaded=px::ui::InstantiateUIScene(scene,nullptr,px::ui::FormatterRegistry{});Check(loaded&&loaded.Value().animations.has_value(),"strict UI scene loader must parse the v5 Animation Library used by Preview and Player");
    if(loaded&&loaded.Value().animations){auto& control=*loaded.Value().root;px::ui::AnimationPlayer player(control);const auto* loadedClip=loaded.Value().animations->FindClip(clipId);Check(loadedClip&&player.Play(*loadedClip),"embedded UI Clip should start");Check(player.Seek(.5f),"UI Clip should scrub to an arbitrary time");Check(std::abs(control.Opacity()-.5f)<.001f,"Editor scrub and runtime playback must use the same typed property interpolation");Check(player.Stop(true),"stopping UI animation should restore the authored value");Check(std::abs(control.Opacity()-1.0f)<.001f,"animation preview must restore the design state");}
    scene.properties["animations"]=px::VariantObject{};Check(!px::ui::InstantiateUIScene(scene,nullptr,px::ui::FormatterRegistry{}),"malformed v5 Animation Libraries must fail strict scene loading");
}

void TestUIAnimationStateMachine(){
    auto root=std::make_unique<px::ui::Control>("AnimationMachine");const px::Uuid target=root->Id();px::ui::UIAnimationController controller(*root);
    const px::Uuid idleClipId=px::Uuid::Random(),hoverClipId=px::Uuid::Random(),pressedClipId=px::Uuid::Random();
    const auto makeClip=[&](const px::Uuid& id,std::string name,double opacity){px::ui::AnimationClip clip;clip.id=id;clip.name=std::move(name);clip.duration=1.0f;clip.tracks.push_back({.node=target,.property="opacity",.keys={{0.0f,opacity,px::ui::Ease::Linear,px::ui::KeyInterpolation::Linear},{1.0f,opacity,px::ui::Ease::Linear,px::ui::KeyInterpolation::Linear}}});return clip;};
    px::ui::UIAnimationLibrary library;library.clips.push_back(makeClip(idleClipId,"Idle",1.0));library.clips.push_back(makeClip(hoverClipId,"Hover",0.0));library.clips.push_back(makeClip(pressedClipId,"Pressed",.25));
    const px::Uuid idle=px::Uuid::Random(),hover=px::Uuid::Random(),pressed=px::Uuid::Random();library.machine.entry=idle;library.machine.states={{idle,"Idle",idleClipId,{0,0}},{hover,"Hover",hoverClipId,{200,0}},{pressed,"Pressed",pressedClipId,{400,0}}};library.machine.parameters={{"go",px::ui::AnimationParameterType::Trigger,false},{"enabled",px::ui::AnimationParameterType::Bool,false},{"speed",px::ui::AnimationParameterType::Number,0.0}};
    const px::Uuid toHover=px::Uuid::Random(),fallback=px::Uuid::Random(),anyPressed=px::Uuid::Random(),exitPressed=px::Uuid::Random();library.machine.transitions={
        {toHover,idle,hover,{{"go",px::ui::AnimationConditionOperator::Triggered,false},{"speed",px::ui::AnimationConditionOperator::GreaterEqual,2.0}},false,1,.2f,10},
        {fallback,idle,pressed,{{"go",px::ui::AnimationConditionOperator::Triggered,false},{"speed",px::ui::AnimationConditionOperator::GreaterEqual,2.0}},false,1,0,1},
        {anyPressed,std::nullopt,pressed,{{"enabled",px::ui::AnimationConditionOperator::Equal,true}},false,1,0,-100},
        {exitPressed,pressed,idle,{},true,.5f,0,0}};
    const px::Variant encoded=px::ui::WriteUIAnimationLibrary(library);auto decoded=px::ui::ParseUIAnimationLibrary(encoded,"animation-machine-test");Check(static_cast<bool>(decoded),"Animation Library must save and load all State Machine data");if(!decoded)return;
    px::Variant duplicateClip=encoded.Clone();auto* duplicateClips=(*duplicateClip.AsObject())["clips"].AsArray();(*duplicateClips->at(1).AsObject())["id"]=idleClipId;Check(!px::ui::ParseUIAnimationLibrary(duplicateClip,"duplicate-clip-test"),"Animation Library must reject duplicate Clip UUIDs");
    px::Variant missingClip=encoded.Clone();auto* missingStates=(*(*missingClip.AsObject())["machine"].AsObject())["states"].AsArray();(*missingStates->at(0).AsObject())["clip"]=px::Uuid::Random();Check(!px::ui::ParseUIAnimationLibrary(missingClip,"missing-clip-test"),"Animation Library must reject State references to missing Clips");
    px::Variant missingParameter=encoded.Clone();auto* missingTransitions=(*(*missingParameter.AsObject())["machine"].AsObject())["transitions"].AsArray();auto* conditions=(*missingTransitions->at(0).AsObject())["conditions"].AsArray();(*conditions->at(0).AsObject())["parameter"]=std::string("missing");Check(!px::ui::ParseUIAnimationLibrary(missingParameter,"missing-parameter-test"),"Animation Library must reject Transition references to missing Parameters");
    Check(controller.SetLibrary(decoded.TakeValue()),"Animation Controller must enter the required Entry state");
    Check(controller.SetTrigger("go"),"Trigger parameter should be set");Check(controller.Update(.01f),"unsatisfied Number condition should not transition");auto state=controller.CaptureState();Check(state.state==idle&&state.parameters.at("go").TryGet<bool>()&&*state.parameters.at("go").TryGet<bool>(),"Trigger must remain armed until a transition succeeds");
    Check(controller.SetNumber("speed",2.0)&&controller.Update(0),"Number >= condition should select the high-priority transition");state=controller.CaptureState();Check(state.state==hover&&state.transition==toHover&&state.parameters.at("go").TryGet<bool>()&&!*state.parameters.at("go").TryGet<bool>(),"successful transition must consume Trigger and preserve transition identity");Check(controller.Update(.1f)&&std::abs(root->Opacity()-.5f)<.06f,"Number properties must cross-fade over Transition Duration");Check(controller.Pause(),"Animation Controller should pause the shared player");const auto blendCheckpoint=controller.CaptureState();Check(blendCheckpoint.paused&&controller.Resume()&&controller.Travel(pressed)&&controller.RestoreState(blendCheckpoint),"paused active transition checkpoints should restore");state=controller.CaptureState();Check(controller.Paused()&&state.transition==toHover&&std::abs(state.transitionProgress-.5f)<.01f&&std::abs(state.position-.1f)<.01f,"pause state, transition progress, and Clip position must survive capture/restore");Check(controller.Resume(),"restored Animation Controller should resume");
    Check(controller.Travel(idle)&&controller.SetTrigger("go")&&controller.SetBool("enabled",true)&&controller.Update(0),"Any State transition should run");state=controller.CaptureState();Check(state.state==pressed&&state.transition==anyPressed&&state.parameters.at("go").TryGet<bool>()&&*state.parameters.at("go").TryGet<bool>(),"Any State must win before current-state Priority and consume only its own Trigger conditions");
    Check(controller.SetBool("enabled",false)&&controller.Update(.4f),"Exit Time update should run");Check(controller.CaptureState().state==pressed,"transition must wait until normalized Exit Time");Check(controller.Update(.2f)&&controller.CaptureState().state==idle,"transition must fire after Exit Time");const auto checkpoint=controller.CaptureState();Check(controller.Travel("Hover")&&controller.RestoreState(checkpoint)&&controller.CaptureState().state==idle,"Animation Controller state and parameters must restore deterministically");
}

void TestUISchemaV5Migration(){
    px::resource::TypedDocument legacy;legacy.kind=px::resource::DocumentKind::Scene;legacy.id=px::Uuid::Random();legacy.type="UIScene";legacy.properties["uiSchemaVersion"]=std::int64_t{4};legacy.properties["animation.duration"]=1.0;legacy.properties["animation.loop"]=false;legacy.properties["animation.tracks"]=px::VariantArray{};
    px::resource::NodeRecord button;button.id=px::Uuid::Random();button.type="Button";button.name="Confirm";button.properties["events"]=px::VariantObject{{"activated",px::VariantObject{{"mode",std::string("action")},{"action",std::string("game.start")},{"arguments",px::VariantObject{}},{"reentry",std::string("Allow")}}}};legacy.nodes.push_back(button);
    auto migrated=px::ui::MigrateUIDocumentV4(legacy,"legacy.pxscene");Check(static_cast<bool>(migrated),"v4 UI document should migrate as a complete in-memory transaction");if(!migrated)return;const auto& current=migrated.Value();Check(current.properties.at("uiSchemaVersion").TryGet<std::int64_t>()&&*current.properties.at("uiSchemaVersion").TryGet<std::int64_t>()==5&&current.properties.contains("animations")&&!current.properties.contains("animation.tracks")&&current.nodes.front().properties.contains("triggers")&&!current.nodes.front().properties.contains("events"),"migration must rename interactions and create Default Clip plus State Machine");
    TempDirectory temp;const auto content=temp.path/"Content"/"UI";std::filesystem::create_directories(content);const auto path=content/"Legacy.pxscene";const std::string text=px::resource::WriteTypedDocument(legacy);{std::ofstream stream(path,std::ios::binary);stream<<text;}auto check=px::ui::MigrateUIProjectV5(temp.path,false);if(!check)for(const auto& diagnostic:check.Diagnostics())std::cerr<<diagnostic.code<<": "<<diagnostic.message<<'\n';Check(check&&check.Value().changed.size()==1,"--check must report pending UI migrations");std::ifstream unchanged(path,std::ios::binary);std::string unchangedText((std::istreambuf_iterator<char>(unchanged)),{});unchanged.close();Check(unchangedText==text,"--check must not write files");auto write=px::ui::MigrateUIProjectV5(temp.path,true);if(!write)for(const auto& diagnostic:write.Diagnostics())std::cerr<<diagnostic.code<<": "<<diagnostic.message<<'\n';Check(write&&write.Value().changed.size()==1,"--write must atomically migrate the project");auto repeat=px::ui::MigrateUIProjectV5(temp.path,false);if(!repeat)for(const auto& diagnostic:repeat.Diagnostics())std::cerr<<diagnostic.code<<": "<<diagnostic.message<<'\n';Check(repeat&&repeat.Value().changed.empty()&&repeat.Value().alreadyCurrent==1,"v5 migration must be safely repeatable");
}

}  // namespace

int main() {
    TestArchiveBoundsAndRoundTrip();
    TestSaveValidation();
    TestTypedUITriggerBinding();
    TestDialogueEffects();
    TestSavedAssetIdentityRegistration();
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
    TestTypedFormatV4TokensAndComponents();
    TestDesignerImageAndTextProperties();
    TestAcceleratedEightHourSoak();
    TestDesignerRewriteContracts();
    TestBehaviorGraphExecutionAndCheckpoint();
    TestExpandedControlMetadataAndTransforms();
    TestEmbeddedUIAnimationParity();
    TestUIAnimationStateMachine();
    TestUISchemaV5Migration();
    if (g_failures == 0) std::cout << "All PrismatiX commercial acceptance tests passed.\n";
    return g_failures == 0 ? 0 : 1;
}
