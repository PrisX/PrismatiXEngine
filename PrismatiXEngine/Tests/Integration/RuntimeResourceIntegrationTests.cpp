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


void TestCjkRubyAndVerticalText() {
    const auto rich = px::text::ParseRubyMarkup("[ruby=かんじ]漢字[/ruby][br]測試");
    Check(rich.plain == "漢字\n測試" && rich.ruby.size() == 1 && rich.ruby.front().reading == "かんじ", "rich text should retain CJK ruby annotations");
    const auto wrapped = px::text::ApplyCjkKinsoku("這是一段測試，不能讓標點出現在行首。", 6);
    Check(wrapped.find("\n，") == std::string::npos && wrapped.find("\n。") == std::string::npos, "CJK wrapping should enforce kinsoku punctuation rules");
    const auto vertical = px::text::LayoutVertical("縱書ABC", 4);
    Check(!vertical.empty() && vertical.back().column > 0 && vertical[2].rotate, "vertical layout should rotate Latin glyphs and advance columns");
}

void TestTypedFormatV4TokensAndComponents() {
    Check(px::ui::RegisterBuiltinUITypes(), "built-in UI metadata should be available for component interface validation");
    px::resource::TypedDocument theme;
    theme.kind = px::resource::DocumentKind::Resource;
    theme.id = px::Uuid::Random();
    theme.type = "UITheme";
    px::ui::StyleThemeData styleData;
    const px::Uuid baseId = px::Uuid::FromName("color.base"), surfaceId = px::Uuid::FromName("color.surface");
    Check(styleData.UpsertToken({ .id = baseId, .displayName = "color.base", .type = px::VariantType::Color, .value = px::ui::StyleValue::Literal(px::Color{ 10, 20, 30, 255 }) }), "base style token should register");
    Check(styleData.UpsertToken({ .id = surfaceId, .displayName = "color.surface", .type = px::VariantType::Color, .value = px::ui::StyleValue::Token(baseId, "color.base") }), "style token alias should register");
    theme.properties["styleSystem"] = px::ui::WriteStyleTheme(styleData);
    const std::string encoded = px::resource::WriteTypedDocument(theme);
    const auto parsed = px::resource::ParseTypedDocument(encoded, "theme.pxtheme");
    Check(parsed && encoded.starts_with("@pxresource 4 "), "typed resources must write and parse strict v4 headers");
    if (parsed) {
        const auto decodedStyle = px::ui::ParseStyleTheme(parsed.Value().properties.at("styleSystem"));
        const auto* alias = decodedStyle ? decodedStyle.Value().FindToken(surfaceId) : nullptr;
        const auto loadedTheme = px::ui::LoadUITheme(parsed.Value());
        const auto resolved = loadedTheme ? loadedTheme.Value().ResolveToken("color.surface") : px::Result<px::Variant>::Failure(loadedTheme.Diagnostics());
        if (!resolved)
            for (const auto& diagnostic : resolved.Diagnostics()) std::cerr << diagnostic.code << ": " << diagnostic.message << " (" << diagnostic.details << ")\n";
        Check(
            alias && alias->value.IsTokenReference() && alias->value.TokenReference() == baseId && resolved && resolved.Value().TryGet<px::Color>() && *resolved.Value().TryGet<px::Color>() == px::Color{ 10, 20, 30, 255 },
            "token() must round-trip and resolve through typed theme aliases"
        );
    }
    std::string legacy = encoded;
    const auto version = legacy.find(" 4 ");
    if (version != std::string::npos) legacy.replace(version, 3, " 3 ");
    Check(!px::resource::ParseTypedDocument(legacy, "legacy-v3.pxtheme"), "typed v3 resources must be rejected without a compatibility parser");

    px::resource::TypedDocument component;
    component.kind = px::resource::DocumentKind::Scene;
    component.id = px::Uuid::Random();
    component.type = "UIComponent";
    component.properties["uiSchemaVersion"] = std::int64_t{ 5 };
    px::resource::NodeRecord root;
    root.id = px::Uuid::Random();
    root.type = "Panel";
    root.name = "Card";
    px::resource::NodeRecord label;
    label.id = px::Uuid::Random();
    label.parent = root.id;
    label.type = "Label";
    label.name = "Caption";
    label.properties["text"] = std::string("Default");
    component.nodes = { root, label };
    component.properties["component.exposedProperties"] =
        px::VariantArray{ px::Variant(px::VariantObject{ { "id", std::string("captionOpacity") }, { "displayName", std::string("Caption Opacity") }, { "node", label.id }, { "property", std::string("opacity") }, { "type", std::string("Number") } }) };
    component.properties["component.exposedSignals"] =
        px::VariantArray{ px::Variant(px::VariantObject{ { "id", std::string("captionClicked") }, { "displayName", std::string("Caption Clicked") }, { "node", label.id }, { "signal", std::string("clicked") } }) };
    component.properties["component.slots"] = px::VariantArray{ px::Variant(px::VariantObject{ { "id", std::string("content") }, { "displayName", std::string("Content") }, { "node", label.id } }) };

    px::resource::TypedDocument scene;
    scene.kind = px::resource::DocumentKind::Scene;
    scene.id = px::Uuid::Random();
    scene.type = "UIScene";
    px::resource::NodeRecord instance;
    instance.id = px::Uuid::Random();
    instance.type = "ComponentInstance";
    instance.name = "Card 1";
    instance.properties["component"] = px::ResourceRefValue{ component.id, "Content/UI/Card.pxcomponent" };
    instance.properties["overrides"] = px::VariantObject{
        { label.id.ToString(), px::VariantObject{ { "text", std::string("Overridden") } } },
    };
    instance.properties["componentProperties"] = px::VariantObject{ { "captionOpacity", 0.4 } };
    instance.properties["componentEvents"] =
        px::VariantObject{ { "captionClicked", px::VariantObject{ { "kind", std::string("action") }, { "action", std::string("game.start") }, { "arguments", px::VariantObject{} }, { "reentry", std::string("Allow") } } } };
    px::resource::NodeRecord slotted;
    slotted.id = px::Uuid::Random();
    slotted.parent = instance.id;
    slotted.type = "Button";
    slotted.name = "Projected";
    slotted.properties["componentSlot"] = std::string("content");
    scene.nodes = { instance, slotted };
    const px::ui::UIDocumentLoader loader = [&component](const px::ResourceRefValue& reference) {
        (void)reference;
        return px::Result<px::resource::TypedDocument>::Success(component);
    };
    const auto first = px::ui::ExpandUIComponents(scene, loader);
    const auto second = px::ui::ExpandUIComponents(scene, loader);
    bool stable = first && second && first.Value().document.nodes.size() == 3 && second.Value().document.nodes.size() == 3;
    if (stable) {
        const auto& expandedRoot = first.Value().document.nodes[0];
        const auto& expandedLabel = first.Value().document.nodes[1];
        const auto& expandedSlotChild = first.Value().document.nodes[2];
        const auto* opacity = expandedLabel.properties.contains("opacity") ? expandedLabel.properties.at("opacity").TryGet<double>() : nullptr;
        const auto* triggers = expandedLabel.properties.contains("triggers") ? expandedLabel.properties.at("triggers").AsObject() : nullptr;
        stable = expandedRoot.id == instance.id && expandedLabel.id == second.Value().document.nodes[1].id && expandedLabel.properties.at("text").TryGet<std::string>() && *expandedLabel.properties.at("text").TryGet<std::string>() == "Overridden" &&
                 opacity && std::abs(*opacity - .4) < .001 && triggers && triggers->contains("clicked") && expandedSlotChild.parent == expandedLabel.id && !expandedSlotChild.properties.contains("componentSlot");
    }
    Check(stable, "component expansion must apply overrides, exposed properties/signals/slots, and stable instance UUIDs");

    scene.nodes.front().properties["componentProperties"] = px::VariantObject{ { "notExposed", true } };
    Check(!px::ui::ExpandUIComponents(scene, loader), "component instances must reject values for properties that are not exposed");
    scene.nodes.front() = instance;

    component.nodes.push_back(instance);
    component.nodes.back().parent = root.id;
    const auto cyclic = px::ui::ExpandUIComponents(scene, loader);
    Check(!cyclic, "nested component dependency cycles must be rejected");
}

void TestDesignerImageAndTextProperties() {
    px::resource::TypedDocument scene;
    scene.kind = px::resource::DocumentKind::Scene;
    scene.id = px::Uuid::Random();
    scene.type = "UIScene";
    scene.properties["uiSchemaVersion"] = std::int64_t{ 5 };
    px::resource::NodeRecord root;
    root.id = px::Uuid::Random();
    root.type = "Panel";
    root.name = "Root";
    px::resource::NodeRecord image;
    image.id = px::Uuid::Random();
    image.parent = root.id;
    image.type = "TextureRect";
    image.name = "Background";
    image.properties = { { "path", std::string("Content/bg.png") }, { "scaleMode", std::string("Fill") }, { "lockAspectRatio", true }, { "editorLocked", true } };
    px::resource::NodeRecord label;
    label.id = px::Uuid::Random();
    label.parent = root.id;
    label.type = "Label";
    label.name = "Centered";
    label.properties = { { "text", std::string("Hello") }, { "horizontalAlignment", std::string("Center") }, { "verticalAlignment", std::string("Bottom") } };
    px::resource::NodeRecord button;
    button.id = px::Uuid::Random();
    button.parent = root.id;
    button.type = "Button";
    button.name = "Action";
    button.properties = { { "text", std::string("Go") }, { "horizontalAlignment", std::string("Right") }, { "verticalAlignment", std::string("Top") } };
    scene.nodes = { root, image, label, button };
    const auto loaded = px::ui::InstantiateUIScene(scene, nullptr, px::ui::FormatterRegistry{});
    bool valid = static_cast<bool>(loaded);
    if (valid) {
        const auto* texture = dynamic_cast<const px::ui::TextureRect*>(loaded.Value().root->Find(image.id));
        const auto* text = dynamic_cast<const px::ui::Label*>(loaded.Value().root->Find(label.id));
        const auto* action = dynamic_cast<const px::ui::Button*>(loaded.Value().root->Find(button.id));
        valid = texture && texture->ScaleMode() == px::ui::TextureScaleMode::Fill && texture->LockAspectRatio() && text && text->HorizontalAlignment() == px::ui::HorizontalTextAlignment::Center &&
                text->VerticalAlignment() == px::ui::VerticalTextAlignment::Bottom && action && action->HorizontalAlignment() == px::ui::HorizontalTextAlignment::Right && action->VerticalAlignment() == px::ui::VerticalTextAlignment::Top;
    }
    Check(valid, "designer image modes, editor-only lock metadata, and text alignment must load through typed UI scenes");
}

void TestDesignerRewriteContracts() {
    px::ui::StyleThemeData theme;
    px::ui::TokenDefinition token{ .id = px::Uuid::FromName("accent"), .displayName = "Accent", .type = px::VariantType::Color, .value = px::ui::StyleValue::Literal(px::Color{ 10, 20, 30, 255 }) };
    Check(theme.UpsertToken(token), "style token should register");
    px::ui::StyleDefinition style;
    style.id = px::Uuid::FromName("primary-style");
    style.displayName = "Primary";
    style.properties["background.color"] = px::ui::StyleValue::Token(token.id, "Accent");
    Check(theme.UpsertStyle(style), "style definition should register");
    px::ui::ControlStyleBinding binding;
    binding.baseStyle = style.id;
    const auto encoded = px::ui::WriteStyleTheme(theme);
    const auto decoded = px::ui::ParseStyleTheme(encoded);
    Check(decoded && decoded.Value().FindToken(token.id) && decoded.Value().FindStyle(style.id), "Style System 3 IDs and definitions must round-trip");
    px::ui::StyleResolveRequest request{ .controlType = "Button", .binding = binding, .activeStates = px::ui::StyleStateSet(px::ui::StyleState::Hover) };
    px::ui::StylePropertyRegistry properties;
    px::ui::StyleResolver resolver;
    const auto resolved = resolver.Resolve(theme, request, properties);
    Check(resolved && resolved.Value().Find("background.color") && resolved.Value().Find("background.color")->tokenChain.size() == 1, "style resolver must preserve token source trace");
    Check(
        properties.RuntimeSupports("background.color", "Button") && properties.RuntimeSupports("padding", "Button") && !properties.RuntimeSupports("opacity", "Button"), "Style Inspector support must match the explicit renderer-backed property allowlist"
    );
    bool styleSupportExact = true;
    const auto mapped = px::ui::RuntimeMappedStyleProperties();
    for (const auto* descriptor : properties.Descriptors()) {
        const bool hasMapper = std::find(mapped.begin(), mapped.end(), descriptor->id) != mapped.end();
        styleSupportExact &= descriptor->runtimeSupported == hasMapper;
    }
    Check(styleSupportExact, "every Style Inspector runtimeSupported flag must have an exact runtime mapper and vice versa");
    Check(px::ui::RegisterBuiltinUITypes(), "UI metadata contracts require registered builtin types");
    const auto* offsets = px::TypeRegistry::Global().FindProperty("Button", "offsets");
    const auto* text = px::TypeRegistry::Global().FindProperty("Button", "text");
    Check(
        offsets && offsets->ownership == px::PropertyOwnership::ParentLayout && px::HasImpact(offsets->impact, px::PropertyImpact::Layout) && text && text->bindable && text->animatable,
        "Inspector layout ownership and edit capabilities must come from PropertyInfo metadata"
    );
    px::ui::ActionInvocation action{ .action = "choice.select", .arguments = { { "index", std::int64_t{ 2 } } } };
    Check(static_cast<bool>(px::ui::ActionCatalog::Global().ValidateAndNormalize(action)), "typed action arguments must validate");
    action.arguments["index"] = std::string("bad");
    Check(!px::ui::ActionCatalog::Global().ValidateAndNormalize(action), "typed action arguments must reject mismatched types");
    px::resource::TypedDocument scene;
    scene.kind = px::resource::DocumentKind::Scene;
    scene.type = "UIScene";
    scene.properties["uiSchemaVersion"] = std::int64_t{ 4 };
    px::resource::NodeRecord button;
    button.id = px::Uuid::Random();
    button.type = "Button";
    scene.nodes.push_back(button);
    Check(!px::ui::InstantiateUIScene(scene, nullptr, px::ui::FormatterRegistry{}), "strict UI schema must reject v4 without a compatibility branch");
    scene.properties["uiSchemaVersion"] = std::int64_t{ 5 };
    scene.nodes[0].properties["command"] = std::string("game.start");
    Check(!px::ui::InstantiateUIScene(scene, nullptr, px::ui::FormatterRegistry{}), "strict UI schema must reject command instead of migrating it");
    scene.nodes[0].properties.erase("command");
    scene.nodes[0].properties["themeVariant"] = std::string("Button");
    Check(!px::ui::InstantiateUIScene(scene, nullptr, px::ui::FormatterRegistry{}), "strict UI schema must reject themeVariant instead of migrating it");
}


void TestExpandedControlMetadataAndTransforms() {
    Check(px::ui::RegisterBuiltinUITypes(), "built-in UI types should register");
    const auto& registry = px::TypeRegistry::Global();
    for (const char* name : { "NinePatchRect", "TextEdit", "OptionButton", "SpinBox", "RadioButton", "Separator", "ScrollBar", "VideoRect" }) {
        const auto* type = registry.Find(name);
        Check(type && type->designer && type->designer->paletteVisible, "new built-in controls must expose palette metadata");
        Check(static_cast<bool>(registry.Create(name)), "new built-in controls must be constructible from metadata");
    }
    Check(registry.FindSignal("OptionButton", "itemSelected") && registry.FindSignal("SpinBox", "valueChanged") && registry.FindSignal("TextEdit", "textChanged"), "new control signals must expose named typed metadata");
    px::ui::Control transformed;
    transformed.Arrange({ 10, 10, 20, 10 });
    transformed.SetPivot({ .5f, .5f });
    transformed.SetScale({ 2, 2 });
    transformed.SetRotation(90);
    Check(transformed.HitTest({ 20, 15 }) && transformed.HitTest({ 20, 30 }) && !transformed.HitTest({ 50, 50 }), "Control hit testing must match pivot, scale, and rotation");
    transformed.SetVisibility(px::ui::Visibility::Hidden);
    Check(!transformed.HitTest({ 20, 15 }), "Hidden controls must not participate in hit testing");
    px::ui::Control visibility;
    visibility.SetCustomMinimumSize({ 40, 20 });
    visibility.SetVisibility(px::ui::Visibility::Hidden);
    const auto hiddenSize = visibility.Measure({ 100, 100 });
    Check(hiddenSize.x == 40 && hiddenSize.y == 20, "Hidden controls must preserve their layout size");
    visibility.SetVisibility(px::ui::Visibility::Collapsed);
    const auto collapsedSize = visibility.Measure({ 100, 100 });
    Check(collapsedSize.x == 0 && collapsedSize.y == 0, "Collapsed controls must be removed from layout");
    px::ui::VideoRect video("Content/Video/intro.webm");
    bool decoderPlaying = false;
    int opens = 0, updates = 0, stops = 0;
    (void)video.ConnectSignal("playbackStopped", [&](const auto&) { ++stops; });
    video.SetPlayback({ [&](std::string_view path) {
                           ++opens;
                           decoderPlaying = path == "Content/Video/intro.webm";
                           return decoderPlaying;
                       },
                        [&] { decoderPlaying = false; },
                        [&](float) {
                            ++updates;
                            decoderPlaying = false;
                        },
                        [&] { return decoderPlaying; },
                        {},
                        {} });
    Check(video.Playing() && opens == 1, "VideoRect autoplay must open its typed video resource through the host decoder");
    video.Update(.1f);
    Check(!video.Playing() && updates == 1 && stops == 1, "VideoRect must publish playback completion instead of remaining a placeholder");
    video.SetLoop(true);
    video.SetPlaying(true);
    video.Update(.1f);
    Check(video.Playing() && opens == 3, "looping VideoRect playback must reopen the same decoder resource");
}


}  // namespace

int main() {
    Run("Typography_CjkRubyVerticalText", TestCjkRubyAndVerticalText);
    Run("UIFormat_TokensAndComponents", TestTypedFormatV4TokensAndComponents);
    Run("Designer_ImageAndTextRuntimeProperties", TestDesignerImageAndTextProperties);
    Run("Designer_CurrentRewriteBoundaries", TestDesignerRewriteContracts);
    Run("Controls_MetadataTransformsAndVideo", TestExpandedControlMetadataAndTransforms);
    if (g_failures == 0) std::cout << "PASS: runtime-resource integration\n";
    return g_failures == 0 ? 0 : 1;
}

