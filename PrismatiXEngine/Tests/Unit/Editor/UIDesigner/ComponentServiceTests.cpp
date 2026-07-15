#include "Editor/Tools/UIDesigner/Components/ComponentService.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/DesignerFixture.h"

int main() {
    px::test::Suite suite("ComponentService");

    suite.Run("InstantiateOverrideResetDetach_UndoRedoThroughOneBoundary", [&] {
        suite.Expect(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                     "component tests register typed UI metadata");
        px::test::DesignerFixture fixture(suite);
        px::editor::ComponentService components;
        const px::Uuid sourceRoot =
            px::Uuid::FromName("PrismatiX.Component.SourceRoot");
        px::resource::TypedDocument source;
        source.kind = px::resource::DocumentKind::Scene;
        source.id = px::Uuid::FromName("PrismatiX.Component.Source");
        source.type = "UIComponent";
        source.properties["uiSchemaVersion"] = std::int64_t{5};
        source.properties["component.exposedProperties"] = px::VariantArray{
            px::VariantObject{{"id", std::string("caption")},
                              {"displayName", std::string("Caption")},
                              {"node", sourceRoot},
                              {"property", std::string("text")},
                              {"type", std::string("String")}}};
        source.nodes.push_back(
            {sourceRoot, {}, "Source Label", "Label", {{"text", std::string("Source")}}});
        components.SetLoader([source](const px::ResourceRefValue&) {
            return px::Result<px::resource::TypedDocument>::Success(source);
        });
        const px::ResourceRefValue reference{source.id,
                                              "Content/UI/Test.pxcomponent"};
        const auto before = fixture.session.Commands().HistoryCursor();
        suite.Expect(static_cast<bool>(components.Instantiate(
                         *fixture.session.Document(), fixture.session.Commands(), reference,
                         fixture.root,
                         fixture.session.DocumentView().Children(fixture.root).size())) &&
                         fixture.session.Commands().HistoryCursor() == before + 1,
                     "Instantiate is one command-backed component edit");
        const auto children = fixture.session.DocumentView().Children(fixture.root);
        const px::Uuid instance = children.back();
        const px::VariantObject values{{"caption", std::string("Hello")}};
        suite.Expect(static_cast<bool>(components.SetInstanceInterface(
                         *fixture.session.Document(), fixture.session.Commands(), instance,
                         "componentProperties", values, "Set public properties")) &&
                         static_cast<bool>(components.AssignSlot(
                             *fixture.session.Document(), fixture.session.Commands(),
                             fixture.child, "content")) &&
                         static_cast<bool>(components.SetPropertyOverride(
                             *fixture.session.Document(), fixture.session.Commands(), instance,
                             sourceRoot, "text", std::string("Override"))) &&
                         components.OverrideCount(*fixture.session.Document(), instance) == 1,
                     "public API, slot, and property override use ComponentService");
        suite.Expect(static_cast<bool>(components.ResetPropertyOverride(
                         *fixture.session.Document(), fixture.session.Commands(), instance,
                         sourceRoot, "text")) &&
                         components.OverrideCount(*fixture.session.Document(), instance) == 0,
                     "Reset override removes the exact override through commands");
        suite.Expect(static_cast<bool>(components.Detach(
                         *fixture.session.Document(), fixture.session.Commands(), instance)) &&
                         fixture.session.Document()->Find(instance)->type !=
                             "ComponentInstance" &&
                         static_cast<bool>(fixture.session.Commands().Undo()) &&
                         fixture.session.Document()->Find(instance)->type ==
                             "ComponentInstance" &&
                         static_cast<bool>(fixture.session.Commands().Redo()) &&
                         fixture.session.Document()->Find(instance)->type !=
                             "ComponentInstance",
                     "Detach and undo/redo preserve stable instance identity");
    });

    suite.Run("InterfaceDefinition_IsOneUndoableComponentApiEdit", [&] {
        px::test::DesignerFixture fixture(suite);
        fixture.session.Document()->Data().type = "UIComponent";
        px::editor::ComponentService components;
        const px::VariantArray definitions{
            px::VariantObject{{"id", std::string("caption")}}};
        const auto before = fixture.session.Commands().HistoryCursor();
        suite.Expect(static_cast<bool>(components.SetInterfaceDefinitions(
                         *fixture.session.Document(), fixture.session.Commands(),
                         "component.exposedProperties", definitions, "Set component API")) &&
                         fixture.session.Commands().HistoryCursor() == before + 1 &&
                         static_cast<bool>(fixture.session.Commands().Undo()) &&
                         !fixture.session.Document()->Data().properties.contains(
                             "component.exposedProperties") &&
                         static_cast<bool>(fixture.session.Commands().Redo()) &&
                         fixture.session.Document()->Data().properties.contains(
                             "component.exposedProperties"),
                     "component API definition is one exact undo/redo edit");
    });

    return suite.Finish();
}
