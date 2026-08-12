#include "Engine/SDK/UiTypeRegistry.h"
#include "Engine/UI/UITypeManifest.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

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

    constexpr std::string_view revisionOneFixture =
        R"({"format":"PrismatiXUiTypeRegistry","schemaRevision":1,"contract":"uiTypeRegistry","contractRevision":1,"contractHash":"0f2044c10f8697034577b1b09a1baa87efecda10901c912b7a4e05a91fff542d","controls":[{"id":"button","runtimeType":"Button","displayName":"Button","description":"","category":"Input","iconId":"ui.button","canHaveChildren":false,"acceptedResourceKinds":[],"properties":[],"signals":[]}]})";
    const auto revisionOne = px::sdk::ParseUiTypeRegistry(revisionOneFixture);
    Check(revisionOne.Valid() && revisionOne.manifest.schemaRevision == 1 &&
              revisionOne.manifest.controls.front().nodeKind == "button",
          "revision-1 manifests must verify before legacy nodeKind normalization");

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
    return 0;
}
