#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "Engine/Animation/Timeline.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/VFS.h"
#include "Engine/Lua/LuaHost.h"
#include "Engine/Platform/Input.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Text/Typography.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/Styles/StyleResolver.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/Widgets.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Expression/Expression.h"
#include "Engine/VN/GameCatalog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Engine/VN/Scenario/StoryMap.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

int g_failures = 0;
std::string_view g_currentTest = "runtime integration setup";

void Check(bool condition, const char* message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAIL [" << g_currentTest << "]\n"
              << "  Expected: " << message << '\n'
              << "  Actual: predicate evaluated false\n";
}
void Check(const px::Status& status, const char* message) { Check(static_cast<bool>(status), message); }

void Run(const std::string_view name, void (*test)()) {
    g_currentTest = name;
    try {
        test();
    } catch (const std::exception& error) {
        ++g_failures;
        std::cerr << "UNCAUGHT [" << name << "]: " << error.what() << '\n';
    } catch (...) {
        ++g_failures;
        std::cerr << "UNCAUGHT [" << name << "]: unknown exception\n";
    }
}


void TestTypedUITriggerBinding() {
    px::resource::TypedDocument document;
    document.kind = px::resource::DocumentKind::Scene;
    document.id = px::Uuid::Random();
    document.type = "UIScene";
    document.properties["uiSchemaVersion"] = std::int64_t{ 5 };
    px::resource::NodeRecord button;
    button.id = px::Uuid::Random();
    button.type = "Button";
    button.name = "Start";
    button.properties["triggers"] = px::VariantObject{ { "activated", px::VariantObject{ { "kind", std::string("action") }, { "action", std::string("game.start") }, { "arguments", px::VariantObject{} }, { "reentry", std::string("Allow") } } } };
    document.nodes.push_back(std::move(button));

    px::ui::FormatterRegistry formatters;
    const auto loaded = px::ui::InstantiateUIScene(document, nullptr, formatters);
    Check(static_cast<bool>(loaded), "typed TriggerBinding scene should load");
    if (loaded) {
        Check(loaded.Value().triggers.size() == 1 && loaded.Value().triggers.front().signal == "activated" && loaded.Value().triggers.front().action == "game.start", "typed TriggerBinding should produce one generic runtime signal handler");
    }
    const auto directTrigger = [](const std::string& action) {
        return px::VariantObject{ { "activated", px::VariantObject{ { "kind", std::string("action") }, { "action", action }, { "arguments", px::VariantObject{} }, { "reentry", std::string("Allow") } } } };
    };
    document.nodes.front().properties["triggers"] = directTrigger("missing.action");
    Check(!px::ui::InstantiateUIScene(document, nullptr, formatters), "UI schema v5 must reject Direct Action Trigger bindings with missing Action ids");
    document.nodes.front().properties["triggers"] =
        px::VariantObject{ { "missingSignal", px::VariantObject{ { "kind", std::string("action") }, { "action", std::string("game.start") }, { "arguments", px::VariantObject{} }, { "reentry", std::string("Allow") } } } };
    Check(!px::ui::InstantiateUIScene(document, nullptr, formatters), "UI schema v5 must reject Trigger bindings to invalid Control Signals");
    px::ui::BehaviorGraph emptyInteraction;
    document.properties["interactionGraph"] = px::ui::WriteBehaviorGraph(emptyInteraction);
    document.nodes.front().properties["triggers"] = px::VariantObject{ { "activated", px::VariantObject{ { "kind", std::string("flow") }, { "entry", px::Uuid::Random() }, { "reentry", std::string("Allow") } } } };
    Check(!px::ui::InstantiateUIScene(document, nullptr, formatters), "UI schema v5 must reject Flow Trigger bindings with missing Entry references");
    document.properties.erase("interactionGraph");
    document.nodes.front().properties["triggers"] = directTrigger("game.start");
    auto duplicate = document.nodes.front();
    duplicate.name = "Duplicate";
    document.nodes.push_back(std::move(duplicate));
    Check(!px::ui::InstantiateUIScene(document, nullptr, formatters), "UI schema v5 must reject duplicate node UUIDs");
    document.nodes.pop_back();
    document.properties.erase("uiSchemaVersion");
    Check(!px::ui::InstantiateUIScene(document, nullptr, formatters), "UI loader must reject documents without semantic schema 5");
    document.properties["uiSchemaVersion"] = std::int64_t{ 5 };
    document.nodes.front().properties["visible"] = true;
    Check(!px::ui::InstantiateUIScene(document, nullptr, formatters), "UI loader must reject removed legacy properties");
}

void TestDialogueEffects() {
    px::vn::Dialogue dialogue;
    dialogue.SetText("A", "Hello", 0, {}, {}, {}, "shake");
    dialogue.Update(100);
    dialogue.Update(350);
    Check(dialogue.State().effect == "shake" && dialogue.State().effectProgress >= 0.24f, "dialogue effect should remain animated after typewriter completion");

    px::ui::GalgameUI hud;
    px::ui::DialoguePresentation presentation;
    presentation.text = "First line";
    Check(hud.ShowHUD(presentation), "HUD should be created for dialogue input regression test");
    px::Input input;
    input.InjectFrame(-1000, -1000, false);
    (void)hud.Update(input, 1280, 720);
    input.InjectFrame(640, 600, true);
    Check(!hud.Update(input, 1280, 720), "non-interactive HUD panels must not consume dialogue advance clicks");
}


void TestCommandSchemaAndScenarioRoundTrip() {
    px::vn::scenario::ScenarioDocument scenario;
    scenario.id = px::Uuid::FromName("scenario-test");
    scenario.name = "Chapter 1";
    px::vn::scenario::ScenarioNode chapter{ px::Uuid::FromName("chapter-node"), "chapter", { { "title", px::Variant("Chapter 1") } } };
    px::vn::scenario::ScenarioNode variable{ px::Uuid::FromName("variable-node"), "var", { { "name", px::Variant("affection") }, { "add", px::Variant(2) } } };
    scenario.entry = chapter.id;
    scenario.nodes = { chapter, variable };
    scenario.edges.push_back({ px::Uuid::FromName("scenario-edge"), chapter.id, "flow", variable.id, "in" });
    const auto validation = px::vn::scenario::ValidateScenario(scenario);
    Check(validation.Valid(), "strict typed Scenario should satisfy the shared command schema");
    const std::string first = px::vn::scenario::WriteScenario(scenario);
    const auto reparsed = px::vn::scenario::ParseScenario(first, "memory.pxscenario");
    Check(static_cast<bool>(reparsed), "Scenario document should parse after serialization");
    if (reparsed) {
        Check(px::vn::scenario::WriteScenario(reparsed.Value()) == first, "Scenario serialization should be deterministic");
        Check(reparsed.Value().nodes.size() == 2 && reparsed.Value().nodes[1].parameters.at("add").Type() == px::VariantType::Integer, "Scenario should preserve typed command parameters");
    }
    const auto program = px::vn::scenario::CompileScenario(scenario);
    Check(program.errors.empty(), "strict Scenario should compile directly to VM instructions");
}

void TestCharacterCatalogExpressions() {
    px::resource::TypedDocument document;
    document.kind = px::resource::DocumentKind::Resource;
    document.id = px::Uuid::Random();
    document.type = "GameCatalog";
    px::resource::NodeRecord character;
    character.id = px::Uuid::Random();
    character.name = "Alice";
    character.type = "Character";
    character.properties["id"] = std::string("alice");
    character.properties["name"] = std::string("愛麗絲");
    character.properties["voiceDirectory"] = std::string("Content/Audio/Voice/Alice");
    character.properties["defaultExpression"] = std::string("neutral");
    px::resource::NodeRecord neutral;
    neutral.id = px::Uuid::Random();
    neutral.parent = character.id;
    neutral.name = "Neutral";
    neutral.type = "CharacterExpression";
    neutral.properties["id"] = std::string("neutral");
    neutral.properties["name"] = std::string("普通");
    neutral.properties["image"] = px::ResourceRefValue{ px::Uuid::FromName("alice-neutral"), "Content/Images/Character/alice_neutral.png" };
    document.nodes = { character, neutral };
    const std::string encoded = px::resource::WriteTypedDocument(document);
    px::vn::GameCatalog catalog;
    Check(catalog.Load(encoded, "Game.pxres"), "GameCatalog should load typed CharacterExpression children");
    const auto image = catalog.ResolveCharacterImage("alice", {});
    Check(image && image->lastKnownPath == "Content/Images/Character/alice_neutral.png", "Character default expression should resolve to its ResourceRef");
    Check(catalog.CharacterDisplayName("alice") == "愛麗絲", "Character id should resolve to the author-facing display name");

    document.nodes.front().properties["defaultExpression"] = std::string("missing");
    px::vn::GameCatalog invalid;
    Check(!invalid.Load(px::resource::WriteTypedDocument(document), "invalid.pxres"), "GameCatalog must reject a missing default expression");
}

void TestTypedExpressions() {
    const auto expression = px::vn::Expression::Binary(
        px::vn::ExpressionOperator::And,
        px::vn::Expression::Binary(px::vn::ExpressionOperator::GreaterEqual, px::vn::Expression::Variable("affection"), px::vn::Expression::Literal(3)),
        px::vn::Expression::Binary(px::vn::ExpressionOperator::Equal, px::vn::Expression::Variable("route"), px::vn::Expression::Literal("alice"))
    );
    const auto variables = [](std::string_view name) -> std::optional<px::vn::Value> {
        if (name == "affection") return px::vn::Value(4);
        if (name == "route") return px::vn::Value("alice");
        return std::nullopt;
    };
    const auto evaluated = px::vn::EvaluateExpression(expression, variables);
    Check(evaluated && evaluated.Value().TryGet<bool>() && *evaluated.Value().TryGet<bool>(), "typed expression should evaluate bool, number, and string operands");

    const px::vn::Value encoded = px::vn::ExpressionToValue(expression);
    const auto decoded = px::vn::ExpressionFromValue(encoded);
    const auto reevaluated = decoded ? px::vn::EvaluateExpression(decoded.Value(), variables) : px::Result<px::vn::Value>{};
    Check(decoded && reevaluated && reevaluated.Value() == evaluated.Value(), "expression AST should survive typed Value serialization");

    px::vn::scenario::ScenarioDocument expressionScenario;
    expressionScenario.id = px::Uuid::FromName("expression-scenario");
    px::vn::scenario::ScenarioNode condition;
    condition.id = px::Uuid::FromName("condition-node");
    condition.command = "branch";
    condition.parameters["expression"] = px::vn::ExpressionToValue(px::vn::Expression::Binary(px::vn::ExpressionOperator::GreaterEqual, px::vn::Expression::Variable("affection"), px::vn::Expression::Literal(3)));
    px::vn::scenario::ScenarioNode end{ px::Uuid::FromName("condition-end"), "chapter", { { "title", std::string("End") } } };
    expressionScenario.entry = condition.id;
    expressionScenario.nodes = { condition, end };
    expressionScenario.edges.push_back({ px::Uuid::FromName("condition-true"), condition.id, "true", end.id, "in" });
    expressionScenario.edges.push_back({ px::Uuid::FromName("condition-false"), condition.id, "false", end.id, "in" });
    Check(px::vn::scenario::ValidateScenario(expressionScenario).Valid(), "typed condition should satisfy the strict shared command schema");
    const auto scenarioProgram = px::vn::scenario::CompileScenario(expressionScenario);
    const auto typedIf = std::find_if(scenarioProgram.code.begin(), scenarioProgram.code.end(), [](const px::vn::Command& command) { return command.type == "branch"; });
    Check(scenarioProgram.errors.empty() && typedIf != scenarioProgram.code.end() && typedIf->FindTyped("expression"), "Scenario IR should compile directly into VM code without a text projection round-trip");

    px::vn::VariableStore store;
    store.SetValue("route", px::vn::Value("alice"));
    store.SetValue("flags", px::vn::Value(px::vn::ValueList{ true, "seen" }), px::vn::VariableScope::Persistent);
    const auto fromStore = store.Evaluate(px::vn::Expression::Binary(px::vn::ExpressionOperator::Equal, px::vn::Expression::Variable("route"), px::vn::Expression::Literal("alice")));
    Check(fromStore && fromStore.Value().TryGet<bool>() && *fromStore.Value().TryGet<bool>() && store.PersistentKeys().contains("flags"), "variable store should retain typed list/map/string values with explicit scope");
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
    const px::vn::scenario::StoryTarget target{ px::Uuid::FromName("story-target"), px::Uuid::FromName("story-entry"), "Content/Scenario/target.pxscenario" };
    Check(source.nodes.front().parameters.empty() && px::vn::scenario::ConnectStoryTarget(source, jump.id, "target", target), "Story Map connection should update the source Scenario explicitly");
    const auto links = px::vn::scenario::DeriveStoryLinks(source);
    const auto program = px::vn::scenario::CompileScenario(source);
    const auto runtimeJump = std::find_if(program.code.begin(), program.code.end(), [](const px::vn::Command& command) { return command.type == "jump"; });
    Check(
        links.size() == 1 && links.front().target.scenario == target.scenario && runtimeJump != program.code.end() && runtimeJump->Get("target").starts_with("Content/Scenario/target.pxscenario#"),
        "Story Map should be derived from ResourceId targets and compile to an entry route"
    );
    Check(px::vn::scenario::DisconnectStoryTarget(source, jump.id, "target") && px::vn::scenario::DeriveStoryLinks(source).empty(), "deleting a Story Map link should clear the explicit source target");
}

px::Variant ContractValue(const px::vn::CommandParameterDescriptor& parameter) {
    switch (parameter.type) {
        case px::VariantType::Null:
            return px::Variant(1);
        case px::VariantType::Bool:
            return px::Variant(false);
        case px::VariantType::Integer:
            return px::Variant(std::int64_t{ 1 });
        case px::VariantType::Number:
            return px::Variant(1.0);
        case px::VariantType::String:
            return px::Variant(parameter.name == "textId" ? px::Uuid::Random().ToString() : std::string("value"));
        case px::VariantType::ResourceRef:
            return px::ResourceRefValue{ px::Uuid::Random(), "Content/test.asset" };
        case px::VariantType::Object:
            return parameter.widget == px::vn::CommandEditorWidget::Expression ? px::vn::ExpressionToValue(px::vn::Expression::Literal(true)) : px::VariantObject{};
        case px::VariantType::Array:
            return px::VariantArray{};
        case px::VariantType::Uuid:
            return px::Uuid::Random();
        default:
            return parameter.defaultValue.Clone();
    }
}

void TestEveryCommandDescriptorContract() {
    for (const auto& descriptor : px::vn::CommandRegistry::Builtins().Descriptors()) {
        px::vn::scenario::ScenarioDocument document;
        document.id = px::Uuid::Random();
        document.name = descriptor.id;
        px::vn::scenario::ScenarioNode node;
        node.id = px::Uuid::Random();
        node.command = descriptor.id;
        for (const auto& parameter : descriptor.parameters)
            if (parameter.required) node.parameters[parameter.name] = ContractValue(parameter);
        if (descriptor.id == "jump" || descriptor.id == "call") node.parameters["target"] = "@" + node.id.ToString();
        document.entry = node.id;
        const auto nodeId = node.id;
        document.nodes.push_back(std::move(node));
        if (descriptor.id == "choice") document.edges.push_back({ px::Uuid::Random(), nodeId, "choice", nodeId, "in" });
        if (descriptor.id == "branch") {
            document.edges.push_back({ px::Uuid::Random(), nodeId, "true", nodeId, "in" });
            document.edges.push_back({ px::Uuid::Random(), nodeId, "false", nodeId, "in" });
        }
        const auto encoded = px::vn::scenario::WriteScenario(document);
        const auto parsed = px::vn::scenario::ParseScenario(encoded, "contract.pxscenario");
        Check(parsed && px::vn::scenario::ValidateScenario(parsed.Value()).Valid(), ("command contract failed: " + descriptor.id).c_str());
    }
}

void TestVisualGraphControlFlowContract() {
    using namespace px::vn::scenario;
    ScenarioDocument document;
    document.id = px::Uuid::Random();
    document.name = "visual flow";
    ScenarioNode first{ px::Uuid::Random(), "choice", { { "textId", std::string("choice-a") }, { "text", std::string("A") } } };
    ScenarioNode second{ px::Uuid::Random(), "choice", { { "textId", std::string("choice-b") }, { "text", std::string("B") } } };
    ScenarioNode resultA{ px::Uuid::Random(), "chapter", { { "title", std::string("A result") } } };
    ScenarioNode resultB{ px::Uuid::Random(), "chapter", { { "title", std::string("B result") } } };
    document.entry = first.id;
    document.nodes = { resultB, first, resultA, second };
    document.edges = { { px::Uuid::Random(), first.id, "flow", second.id, "in" }, { px::Uuid::Random(), first.id, "choice", resultA.id, "in" }, { px::Uuid::Random(), second.id, "choice", resultB.id, "in" } };
    Check(ValidateScenario(document).Valid(), "linked visual Choice nodes should validate");
    const auto program = CompileScenario(document);
    std::vector<const px::vn::Command*> choices;
    for (const auto& command : program.code)
        if (command.type == "choice") choices.push_back(&command);
    Check(
        choices.size() == 2 && choices[0]->Get("text") == "A" && choices[1]->Get("text") == "B" && !choices[0]->Get("target").empty() && !choices[1]->Get("target").empty(),
        "visual Choice flow order and branch targets must compile independently of storage order"
    );

    ScenarioDocument branchDocument;
    branchDocument.id = px::Uuid::Random();
    branchDocument.name = "branch";
    ScenarioNode branch{ px::Uuid::Random(), "branch", { { "expression", px::vn::ExpressionToValue(px::vn::Expression::Literal(true)) } } };
    branchDocument.entry = branch.id;
    branchDocument.nodes = { branch, resultA, resultB };
    branchDocument.edges = { { px::Uuid::Random(), branch.id, "true", resultA.id, "in" }, { px::Uuid::Random(), branch.id, "false", resultB.id, "in" } };
    const auto branchProgram = CompileScenario(branchDocument);
    const auto runtimeBranch = std::find_if(branchProgram.code.begin(), branchProgram.code.end(), [](const auto& command) { return command.type == "branch"; });
    Check(ValidateScenario(branchDocument).Valid() && runtimeBranch != branchProgram.code.end() && !runtimeBranch->Get("trueTarget").empty() && !runtimeBranch->Get("falseTarget").empty(), "If True/False ports must compile to explicit runtime targets");
}


}  // namespace

int main() {
    Run("UITrigger_TypedBindingDispatch", TestTypedUITriggerBinding);
    Run("Dialogue_EffectsReachRuntimeState", TestDialogueEffects);
    Run("Scenario_CommandSchemaRoundTrip", TestCommandSchemaAndScenarioRoundTrip);
    Run("Catalog_CharacterExpressions", TestCharacterCatalogExpressions);
    Run("Expression_TypedEvaluation", TestTypedExpressions);
    Run("StoryMap_SerializedGraph", TestStoryMap);
    Run("Scenario_EveryCommandDescriptor", TestEveryCommandDescriptorContract);
    Run("Scenario_VisualGraphControlFlow", TestVisualGraphControlFlowContract);
    if (g_failures == 0) std::cout << "PASS: dialogue-scenario integration\n";
    return g_failures == 0 ? 0 : 1;
}

