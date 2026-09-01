#include "Engine/Core/TypeRegistry.h"
#include "Engine/SDK/UiTypeRegistry.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/UITypeManifest.h"
#include "Engine/UI/UITypeRegistry.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

class TestExtensionControl final : public px::ui::Control {
public:
    [[nodiscard]] std::string_view TypeName() const override {
        return "TestExtensionControl";
    }
};

px::TypeInfo ExtensionType(std::string category) {
    px::TypeInfo type{
        "TestExtensionControl", "Control", std::move(category),
        [] { return std::make_unique<TestExtensionControl>(); }};
    type.designer=px::DesignerTypeMetadata{
        .displayName="Test extension control",
        .description="Source-owned integration fixture",
        .category="Test",
        .iconId="ui.test-extension",
        .defaultSize={200.0f,60.0f},
        .canHaveChildren=false,
        .paletteVisible=true};
    return type;
}

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

const px::sdk::UiTypeRegistryControl* FindControl(
    const px::sdk::UiTypeRegistryManifest& manifest,
    const std::string_view id) {
    for (const auto& control : manifest.controls)
        if (control.id == id) return &control;
    return nullptr;
}

const px::sdk::UiTypeRegistryProperty* FindProperty(
    const px::sdk::UiTypeRegistryControl& control,
    const std::string_view id) {
    for (const auto& property : control.properties)
        if (property.id == id) return &property;
    return nullptr;
}

}  // namespace

int main() {
    const auto first = px::ui::BuildUiTypeRegistryManifest();
    const auto second = px::ui::BuildUiTypeRegistryManifest();
    Check(first && second && first.Value() == second.Value(),
          "UI TypeRegistry export must be deterministic");
    const auto parsed = px::sdk::ParseUiTypeRegistry(first.Value());
    Check(parsed.Valid() && parsed.manifest.controls.size() > 9,
          "production Studio control mappings must export dynamic palette controls");
    const auto* button = FindControl(parsed.manifest, "button");
    Check(button && button->runtimeType == "Button" &&
              button->nodeKind == "button",
          "Studio button must be derived from the Runtime Button");
    const auto* opacity = button ? FindProperty(*button, "opacity") : nullptr;
    Check(opacity && opacity->writable && opacity->animatable &&
              opacity->tokenBindable &&
              opacity->hasRange && opacity->range.minimum == 0.0 &&
              opacity->range.maximum == 1.0,
          "Runtime opacity metadata must reach the SDK contract");
    const auto* horizontal =
        button ? FindProperty(*button, "horizontalAlignment") : nullptr;
    Check(horizontal && horizontal->enumChoices.size() == 3,
          "Runtime enum editor choices must reach the SDK contract");
    const auto* styleToken = button ? FindProperty(*button, "styleToken") : nullptr;
    Check(styleToken && styleToken->valueType == "token" && styleToken->writable,
          "Control style-token references must expose a real TokenRef setter");
    const auto* image = FindControl(parsed.manifest, "image");
    const auto* texture = image ? FindProperty(*image, "texture") : nullptr;
    Check(texture && texture->valueType == "resource" && texture->writable &&
              texture->resourceFilter == "image" &&
              texture->defaultValueJson == "\"\"",
          "TextureRect must expose a UUID-authoritative filtered ResourceRef setter");
    const auto* optionButton = FindControl(parsed.manifest, "optionButton");
    const auto* options = optionButton
                              ? FindProperty(*optionButton, "options")
                              : nullptr;
    Check(optionButton && optionButton->runtimeType == "OptionButton" &&
              optionButton->nodeKind == "leaf" && options &&
              options->valueType == "array" &&
              options->defaultValueJson == "[]",
          "OptionButton must expose leaf identity and its stable Array default");
    const auto* richText = FindControl(parsed.manifest, "richTextLabel");
    const auto* vertical = richText ? FindProperty(*richText, "vertical") : nullptr;
    const auto* verticalRows = richText
                                   ? FindProperty(*richText, "verticalRows")
                                   : nullptr;
    Check(richText && vertical && vertical->valueType == "boolean" &&
              vertical->writable && verticalRows && verticalRows->hasRange,
          "RichTextLabel vertical writing and ruby layout controls must reach the generated SDK registry");
    const auto* edgeReveal = FindControl(parsed.manifest, "edgeRevealContainer");
    const auto* revealEasing = edgeReveal
                                   ? FindProperty(*edgeReveal, "revealEasing")
                                   : nullptr;
    Check(revealEasing && revealEasing->writable &&
              revealEasing->enumChoices ==
                  std::vector<std::string>({"Linear", "EaseOut", "EaseInOut"}),
          "EdgeRevealContainer easing choices must reach the generated SDK registry");

    constexpr std::string_view revisionOneFixture =
        R"({"format":"PrismatiXUiTypeRegistry","schemaRevision":1,"contract":"uiTypeRegistry","contractRevision":1,"contractHash":"0f2044c10f8697034577b1b09a1baa87efecda10901c912b7a4e05a91fff542d","controls":[{"id":"button","runtimeType":"Button","displayName":"Button","description":"","category":"Input","iconId":"ui.button","canHaveChildren":false,"acceptedResourceKinds":[],"properties":[],"signals":[]}]})";
    const auto revisionOne = px::sdk::ParseUiTypeRegistry(revisionOneFixture);
    Check(!revisionOne.Valid(),
          "revision-1 UI TypeRegistry manifests must be migrated before 0.2 consumption");

    auto tampered = first.Value();
    const auto display = tampered.find("\"Button\"");
    Check(display != std::string::npos, "fixture must contain Button");
    tampered.replace(display, 8, "\"Tampered\"");
    Check(!px::sdk::ParseUiTypeRegistry(tampered).Valid(),
          "tampered UI TypeRegistry hash must be rejected");

    std::string oversized(4 * 1024 * 1024 + 1, 'x');
    Check(!px::sdk::ParseUiTypeRegistry(oversized).Valid(),
          "oversized UI TypeRegistry manifest must be rejected");

    auto future = first.Value();
    const auto revision = future.find("\"schemaRevision\": 2");
    Check(revision != std::string::npos, "fixture must expose schema revision");
    future.replace(revision, 19, "\"schemaRevision\": 3");
    Check(!px::sdk::ParseUiTypeRegistry(future).Valid(),
          "future UI TypeRegistry revision must be rejected");

    auto& registry=px::TypeRegistry::Global();
    Check(px::ui::RegisterBuiltinUITypes().IsOk(),
          "source-owned type lifecycle requires builtin Control metadata");
    Check(registry.ReplaceSource("test.extension.ui",{
              ExtensionType("UI/Test/V1")}).IsOk(),
          "an extension UI type source must install atomically");
    auto created=registry.Create("TestExtensionControl");
    const auto extensionManifest=px::ui::BuildUiTypeRegistryManifest();
    const auto parsedExtension=extensionManifest
        ?px::sdk::ParseUiTypeRegistry(extensionManifest.Value())
        :px::sdk::UiTypeRegistryParseResult{};
    Check(created && created->TypeName()=="TestExtensionControl" &&
              FindControl(parsedExtension.manifest,"testExtensionControl"),
          "custom controls must reach construction and the generated Authoring registry");

    auto invalid=ExtensionType("UI/Test/Broken");
    invalid.properties.push_back({"duplicate","Test",px::VariantType::String,
        px::PropertyFlags::Editable,px::Variant(std::string{})});
    invalid.properties.push_back({"duplicate","Test",px::VariantType::String,
        px::PropertyFlags::Editable,px::Variant(std::string{})});
    Check(!registry.ReplaceSource("test.extension.ui",{std::move(invalid)}) &&
              registry.Find("TestExtensionControl") &&
              registry.Find("TestExtensionControl")->category=="UI/Test/V1",
          "a failed hot reload must retain the complete prior type source");
    Check(registry.ReplaceSource("test.extension.ui",{
              ExtensionType("UI/Test/V2")}) &&
              registry.Find("TestExtensionControl")->category=="UI/Test/V2",
          "a valid hot reload must replace one source as a unit");

    px::TypeInfo dependent{
        "TestExtensionDependent", "TestExtensionControl", "UI/Test",
        [] { return std::make_unique<TestExtensionControl>(); }};
    Check(registry.ReplaceSource("test.extension.dependent",{
              std::move(dependent)}) &&
              !registry.RemoveSource("test.extension.ui") &&
              registry.Find("TestExtensionControl"),
          "unload must reject a source while foreign registered types depend on it");
    Check(registry.RemoveSource("test.extension.dependent") &&
              registry.RemoveSource("test.extension.ui") &&
              !registry.Find("TestExtensionControl") &&
              registry.TypesForSource("test.extension.ui").empty(),
          "source unload must revoke every custom type without ghost registrations");
    return 0;
}
