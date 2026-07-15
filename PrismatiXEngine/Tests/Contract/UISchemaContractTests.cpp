#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

px::resource::TypedDocument Scene() {
    px::resource::TypedDocument scene;
    scene.kind = px::resource::DocumentKind::Scene;
    scene.id = px::Uuid::FromName("PrismatiX.Contract.UIScene");
    scene.type = "UIScene";
    scene.properties["uiSchemaVersion"] = std::int64_t{5};
    scene.properties["canvasSize"] = px::Vec2{1280, 720};
    scene.nodes.push_back({px::Uuid::FromName("PrismatiX.Contract.UIRoot"), {},
                           "Root", "Control", {}});
    return scene;
}

}  // namespace

int main() {
    px::test::Suite suite("UISchemaContract");

    suite.Run("CurrentV5_RoundTripsAndInstantiatesTypedScene", [&] {
        suite.Expect(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                     "built-in UI metadata registers");
        const auto authored = Scene();
        const auto text = px::resource::WriteTypedDocument(authored);
        const auto parsed = px::resource::ParseTypedDocument(text, "current.pxscene");
        suite.Expect(parsed && parsed.Value().properties.at("uiSchemaVersion") ==
                                   px::Variant(std::int64_t{5}) &&
                         static_cast<bool>(px::ui::InstantiateUIScene(
                             parsed.Value(), nullptr, px::ui::FormatterRegistry{})),
                     "current strict v5 scene round-trips and instantiates without migration");
    });

    suite.Run("RemovedLegacyProperties_AreRejectedInsteadOfMigrated", [&] {
        suite.Expect(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                     "built-in UI metadata registers for strict rejection");
        auto scene = Scene();
        scene.properties["uiSchemaVersion"] = std::int64_t{4};
        suite.Expect(!px::ui::InstantiateUIScene(scene, nullptr,
                                                 px::ui::FormatterRegistry{}),
                     "v4 is rejected with no compatibility branch");
        scene.properties["uiSchemaVersion"] = std::int64_t{5};
        for (const char* legacy : {"command", "themeVariant", "zOrder"}) {
            scene.nodes.front().properties.clear();
            scene.nodes.front().properties[legacy] = std::string("legacy");
            suite.Expect(!px::ui::InstantiateUIScene(scene, nullptr,
                                                     px::ui::FormatterRegistry{}),
                         std::string("removed property is rejected: ") + legacy);
        }
    });

    suite.Run("TypedTriggerBinding_LoadsOnlyKnownSignalAndActionArguments", [&] {
        suite.Expect(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                     "built-in UI metadata registers for trigger contract");
        auto scene = Scene();
        auto& button = scene.nodes.front();
        button.type = "Button";
        button.properties["triggers"] = px::VariantObject{
            {"activated",
             px::VariantObject{{"kind", std::string("action")},
                               {"action", std::string("choice.select")},
                               {"arguments", px::VariantObject{{"index", std::int64_t{2}}}},
                               {"reentry", std::string("Allow")}}}};
        const auto loaded = px::ui::InstantiateUIScene(
            scene, nullptr, px::ui::FormatterRegistry{});
        suite.Expect(loaded && loaded.Value().triggers.size() == 1 &&
                         loaded.Value().triggers.front().action == "choice.select",
                     "typed current trigger survives loader validation");
        (*button.properties["triggers"].AsObject())["missingSignal"] =
            px::VariantObject{{"kind", std::string("action")},
                              {"action", std::string("choice.select")},
                              {"arguments", px::VariantObject{{"index", std::int64_t{2}}}}};
        suite.Expect(!px::ui::InstantiateUIScene(scene, nullptr,
                                                 px::ui::FormatterRegistry{}),
                     "unknown signal is rejected by typed schema contract");
    });

    return suite.Finish();
}
