#include "Engine/Core/TypeRegistry.h"
#include "Engine/SDK/UiTypeRegistry.h"
#include "Engine/UI/UITypeManifest.h"
#include "Engine/UI/UITypeRegistry.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

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
    const auto* translation=button?FindProperty(*button,"translation"):nullptr;
    Check(translation&&translation->valueType=="vec2"&&translation->writable&&
              translation->animatable,
          "Runtime translation must be authorable and animatable through the SDK contract");
    const auto* hoverSound=button?FindProperty(*button,"hoverSoundPath"):nullptr;
    const auto* activateSound=button?FindProperty(*button,"activateSoundPath"):nullptr;
    Check(hoverSound&&hoverSound->writable&&activateSound&&activateSound->writable,
          "Controls must expose authored hover and activation sounds");
    const auto* bindingAction = button ? FindProperty(*button, "bindingAction") : nullptr;
    const auto* escapeCancels = button ? FindProperty(*button, "escapeCancelsBinding") : nullptr;
    const auto* selectableKeys = button ? FindProperty(*button, "selectableBindingKeys") : nullptr;
    const auto* confirmShared = button ? FindProperty(*button, "confirmSharedBinding") : nullptr;
    const auto* confirmText = button ? FindProperty(*button, "bindingConfirmText") : nullptr;
    Check(bindingAction && bindingAction->writable &&
              bindingAction->valueType == "string" && escapeCancels &&
              escapeCancels->writable && escapeCancels->valueType == "boolean" &&
              selectableKeys && selectableKeys->writable &&
              selectableKeys->valueType == "boolean" &&
              confirmShared && confirmShared->writable &&
              confirmShared->valueType == "boolean" && confirmText &&
              confirmText->writable && confirmText->valueType == "string",
          "button key-capture options must reach the SDK UI registry");
    const auto* bindingDevice = button ? FindProperty(*button, "bindingDevice") : nullptr;
    const auto* wheelThreshold = button ? FindProperty(*button, "wheelThreshold") : nullptr;
    const auto* wheelUpText = button ? FindProperty(*button, "wheelUpText") : nullptr;
    const auto* wheelDownText = button ? FindProperty(*button, "wheelDownText") : nullptr;
    Check(bindingDevice && bindingDevice->writable &&
              bindingDevice->enumChoices.size() == 2 && wheelThreshold &&
              wheelThreshold->writable && wheelThreshold->hasRange &&
              wheelThreshold->range.minimum == 0.0 &&
              wheelThreshold->range.maximum == 100.0 &&
              wheelUpText && wheelUpText->writable &&
              wheelDownText && wheelDownText->writable,
          "wheel capture settings must reach the SDK UI registry");
    const auto* textureButton = FindControl(parsed.manifest, "textureButton");
    const auto* normalPath = textureButton
                                 ? FindProperty(*textureButton, "normalPath")
                                 : nullptr;
    Check(textureButton && textureButton->runtimeType == "TextureButton" &&
              normalPath && normalPath->writable &&
              normalPath->resourceFilter == "image",
          "TextureButton must be in the exported SDK registry for authored UI validation");
    const auto* slider = FindControl(parsed.manifest, "slider");
    const auto* railHover = slider ? FindProperty(*slider, "railHoverPath") : nullptr;
    const auto* railPressed = slider ? FindProperty(*slider, "railPressedPath") : nullptr;
    Check(slider && railHover && railHover->writable &&
              railHover->resourceFilter == "image" && railPressed &&
              railPressed->writable && railPressed->resourceFilter == "image",
          "Slider rail interaction skins must be authorable through the SDK registry");
    const auto* visible = button ? FindProperty(*button, "visible") : nullptr;
    Check(visible && visible->writable && visible->valueType == "boolean",
          "JSX visible must reach Runtime as a writable boolean property");
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
    const auto* label = FindControl(parsed.manifest, "label");
    const auto* outlineSize = label ? FindProperty(*label, "outlineSize") : nullptr;
    const auto* outlineColor = label ? FindProperty(*label, "outlineColor") : nullptr;
    Check(outlineSize && outlineSize->writable && outlineSize->hasRange &&
              outlineColor && outlineColor->writable &&
              outlineColor->valueType == "color",
          "Label text outline must be authorable through the SDK registry");
    Check(outlineSize && outlineSize->range.maximum == 12.0,
          "Label outlines go up to 12 pixels");
    for (const char* name : {"textShadowColor", "textGradientFrom", "textGradientTo"}) {
        const auto* property = label ? FindProperty(*label, name) : nullptr;
        Check(property && property->writable && property->valueType == "color",
              "Label text effect colours must be authorable through the SDK registry");
    }
    for (const auto& [name, low, high] : {std::tuple{"textShadowOffsetX", -64.0, 64.0},
                                          std::tuple{"textShadowOffsetY", -64.0, 64.0},
                                          std::tuple{"textShadowBlur", 0.0, 8.0},
                                          std::tuple{"fauxBold", 0.0, 4.0},
                                          std::tuple{"fauxItalic", -20.0, 20.0}}) {
        const auto* property = label ? FindProperty(*label, name) : nullptr;
        Check(property && property->writable && property->hasRange &&
                  property->range.minimum == low && property->range.maximum == high,
              "Label text effect numbers must carry their editor ranges");
    }
    const auto* gradientDirection = label ? FindProperty(*label, "textGradientDirection") : nullptr;
    Check(gradientDirection && gradientDirection->writable &&
              gradientDirection->enumChoices == std::vector<std::string>({"vertical", "horizontal"}),
          "Label gradient direction must offer vertical and horizontal");
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
    const auto* revealDistance = edgeReveal
                                     ? FindProperty(*edgeReveal, "revealDistance")
                                     : nullptr;
    const auto* fadeWithReveal = edgeReveal
                                     ? FindProperty(*edgeReveal, "fadeWithReveal")
                                     : nullptr;
    Check(revealDistance && revealDistance->writable &&
              revealDistance->valueType == "number" &&
              fadeWithReveal && fadeWithReveal->writable &&
              fadeWithReveal->valueType == "boolean",
          "EdgeRevealContainer distance and fade must be authorable");

    constexpr std::string_view revisionOneFixture =
        R"({"format":"PrismatiXUiTypeRegistry","schemaRevision":1,"contract":"uiTypeRegistry","contractRevision":1,"contractHash":"0f2044c10f8697034577b1b09a1baa87efecda10901c912b7a4e05a91fff542d","controls":[{"id":"button","runtimeType":"Button","displayName":"Button","description":"","category":"Input","iconId":"ui.button","canHaveChildren":false,"acceptedResourceKinds":[],"properties":[],"signals":[]}]})";
    const auto revisionOne = px::sdk::ParseUiTypeRegistry(revisionOneFixture);
    Check(!revisionOne.Valid(),
          "revision-1 UI TypeRegistry manifests must be migrated before consumption");

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
    const auto revision = future.find("\"schemaRevision\": 1");
    Check(revision != std::string::npos, "fixture must expose schema revision");
    future.replace(revision, 19, "\"schemaRevision\": 2");
    Check(!px::sdk::ParseUiTypeRegistry(future).Valid(),
          "future UI TypeRegistry revision must be rejected");

    return 0;
}
