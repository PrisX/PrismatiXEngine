#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <SDL3/SDL_scancode.h>

#include "Engine/Animation/Timeline.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/VFS.h"
#include "Engine/Platform/Input.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Text/Typography.h"
#include "Engine/Text/TextLayout.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/InputRouter.h"
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

void TestVfsRejectsDirectoriesAndReadsRegularFiles() {
    px::test::TempDirectory fixture("vfs-directory-read");
    std::ofstream(fixture.path / "empty.bin", std::ios::binary);
    {
        std::ofstream output(fixture.path / "payload.bin", std::ios::binary);
        output << "payload";
    }

    px::io::VFS vfs;
    vfs.MountDirectory(fixture.path.string());
    Check(!vfs.Exists("") && !vfs.Read("").has_value(),
          "VFS must not treat a mounted directory as a readable resource");
    Check(!vfs.Exists("missing.bin") && !vfs.Read("missing.bin").has_value(),
          "VFS must report missing directory resources without allocating");
    const auto empty = vfs.Read("empty.bin");
    Check(empty.has_value() && empty->empty(),
          "VFS must preserve empty regular files");
    const auto payload = vfs.ReadText("payload.bin");
    Check(payload.has_value() && *payload == "payload",
          "VFS must read the complete regular-file payload");
    for (const std::string_view hostile : {
             "../payload.bin", "folder/../payload.bin", "./payload.bin",
             "/payload.bin", "C:/payload.bin", "folder\\payload.bin",
             "folder//payload.bin", "//server/share/file.bin",
             "\\\\server\\share\\file.bin", "folder/:stream"}) {
        Check(!px::io::VFS::NormalizeVirtualPath(hostile).has_value() &&
                  !vfs.Exists(hostile) && !vfs.Read(hostile).has_value(),
              "VFS must reject non-canonical or escaping virtual paths");
    }
    const std::string invalidUtf8("bad\xC0\xAF.bin", 9);
    Check(!px::io::VFS::NormalizeVirtualPath(invalidUtf8).has_value(),
          "VFS must reject malformed UTF-8 virtual paths");
    const std::string embeddedNul("payload.bin\0hidden", 18);
    Check(!px::io::VFS::NormalizeVirtualPath(embeddedNul).has_value() &&
              !vfs.Read(embeddedNul).has_value(),
          "VFS must reject embedded NUL paths before host filesystem I/O");

    px::test::TempDirectory outside("vfs-symlink-outside");
    {
        std::ofstream output(outside.path / "secret.bin", std::ios::binary);
        output << "outside";
    }
    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        outside.path, fixture.path / "escape", linkError);
    if (!linkError) {
        Check(!vfs.Exists("escape/secret.bin") &&
                  !vfs.Read("escape/secret.bin").has_value(),
              "directory symlinks escaping a mount root must be rejected");
    }
    linkError.clear();
    std::filesystem::create_symlink(
        outside.path / "secret.bin", fixture.path / "secret-link.bin",
        linkError);
    if (!linkError) {
        Check(!vfs.Exists("secret-link.bin") &&
                  !vfs.Read("secret-link.bin").has_value(),
              "file symlinks escaping a mount root must be rejected");
    }
}


void TestCjkRubyAndVerticalText() {
    const auto rich = px::text::ParseRubyMarkup("[ruby=かんじ]漢字[/ruby][br]測試");
    Check(rich.plain == "漢字\n測試" && rich.ruby.size() == 1 && rich.ruby.front().reading == "かんじ", "rich text should retain CJK ruby annotations");
    const auto wrapped = px::text::ApplyCjkKinsoku("這是一段測試，不能讓標點出現在行首。", 6);
    Check(wrapped.find("\n，") == std::string::npos && wrapped.find("\n。") == std::string::npos, "CJK wrapping should enforce kinsoku punctuation rules");
    const auto vertical = px::text::LayoutVertical("縱書ABC", 4);
    Check(!vertical.empty() && vertical.back().column > 0 && vertical[2].rotate, "vertical layout should rotate Latin glyphs and advance columns");
    const std::string clusters = "👩‍💻é🇯🇵";
    const auto boundaries = px::text::GraphemeBoundaries(clusters);
    Check(boundaries.size() == 4 && boundaries.front() == 0 &&
              boundaries.back() == clusters.size(),
          "emoji ZWJ, combining marks, and regional-indicator pairs must each form one grapheme");
    const auto cjkBreaks = px::text::LineBreakBoundaries("漢字", "ja");
    Check(std::ranges::find(cjkBreaks, std::string("漢").size()) !=
              cjkBreaks.end(),
          "Unicode line breaking must expose the CJK ideograph boundary");
    const std::string nonBreaking = "A\xC2\xA0" "B";
    const auto nonBreakingOffsets =
        px::text::LineBreakBoundaries(nonBreaking, "en");
    Check(nonBreakingOffsets.size() == 2 &&
              nonBreakingOffsets.front() == 0 &&
              nonBreakingOffsets.back() == nonBreaking.size(),
          "Unicode line breaking must preserve no-break-space sequences");
    px::ui::LineEdit line("A" + clusters);
    px::ui::UIEvent backspace{.type = px::ui::UIEventType::KeyDown};
    backspace.key = SDL_SCANCODE_BACKSPACE;
    line.HandleEvent(backspace);
    line.HandleEvent(backspace);
    Check(line.Text() == "A👩‍💻",
          "LineEdit backspace must delete complete grapheme clusters");
    px::ui::TextEdit editor(clusters);
    px::ui::UIEvent home{.type = px::ui::UIEventType::KeyDown};
    home.key = SDL_SCANCODE_HOME;
    editor.HandleEvent(home);
    px::ui::UIEvent remove{.type = px::ui::UIEventType::KeyDown};
    remove.key = SDL_SCANCODE_DELETE;
    editor.HandleEvent(remove);
    Check(editor.Text() == "é🇯🇵",
          "TextEdit delete must not split an emoji ZWJ sequence");
}

void TestImmutableTextLayoutRanges() {
    px::text::TextLayout layout(
        "A中", "en-US", {30, 20},
        {{0, 1, 0, {0, 0, 10, 20}, false, false},
         {1, 3, 0, {10, 0, 20, 20}, false, false}});
    Check(layout.Locale() == "en-US" && layout.Size() == px::Vec2{30, 20},
          "text layout must retain the shaping locale and immutable extent");
    Check(layout.ByteOffsetAt({4, 10}) == 0 &&
              layout.ByteOffsetAt({8, 10}) == 1 &&
              layout.ByteOffsetAt({25, 10}) == 4,
          "text hit testing must return stable UTF-8 byte boundaries");
    const px::Rect range = layout.BoundsForRange(1, 3);
    Check(range == px::Rect{10, 0, 20, 20} &&
              layout.CaretPosition(1) == px::Vec2{10, 0} &&
              layout.CaretPosition(4) == px::Vec2{30, 0},
          "ruby, selection, and caret consumers must share cluster geometry");
    px::text::TextLayout rtl(
        "א", "he", {12, 20},
        {{0, 2, 0, {0, 0, 12, 20}, false, true}});
    Check(rtl.CaretPosition(0) == px::Vec2{12, 0} &&
              rtl.CaretPosition(2) == px::Vec2{0, 0},
          "RTL caret edges must follow shaped cluster direction");
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
    px::resource::NodeRecord rich;
    rich.id = px::Uuid::Random();
    rich.parent = root.id;
    rich.type = "RichTextLabel";
    rich.name = "VerticalRuby";
    rich.properties = {{"markup", std::string("[ruby=かんじ]漢字[/ruby]")},
                       {"vertical", true}, {"verticalRows", std::int64_t{8}}};
    scene.nodes = { root, image, label, button, rich };
    const auto loaded = px::ui::InstantiateUIScene(scene, nullptr, px::ui::FormatterRegistry{});
    bool valid = static_cast<bool>(loaded);
    if (valid) {
        const auto* texture = dynamic_cast<const px::ui::TextureRect*>(loaded.Value().root->Find(image.id));
        const auto* text = dynamic_cast<const px::ui::Label*>(loaded.Value().root->Find(label.id));
        const auto* action = dynamic_cast<const px::ui::Button*>(loaded.Value().root->Find(button.id));
        const auto* verticalRuby = dynamic_cast<const px::ui::RichTextLabel*>(loaded.Value().root->Find(rich.id));
        valid = texture && texture->ScaleMode() == px::ui::TextureScaleMode::Fill && texture->LockAspectRatio() && text && text->HorizontalAlignment() == px::ui::HorizontalTextAlignment::Center &&
                text->VerticalAlignment() == px::ui::VerticalTextAlignment::Bottom && action && action->HorizontalAlignment() == px::ui::HorizontalTextAlignment::Right && action->VerticalAlignment() == px::ui::VerticalTextAlignment::Top && verticalRuby && verticalRuby->Vertical() && verticalRuby->VerticalRows() == 8 && verticalRuby->Text() == "漢字";
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
    px::ui::Control clippedRoot("ClippedRoot");
    clippedRoot.Arrange({0, 0, 50, 50});
    clippedRoot.SetClipContent(true);
    auto clippedChild = std::make_unique<px::ui::Control>("ClippedChild");
    clippedChild->Arrange({40, 40, 30, 30});
    auto* clippedChildPointer = clippedChild.get();
    Check(clippedRoot.AddChild(std::move(clippedChild)),
          "clipped hit-test fixture should attach its child");
    px::ui::InputRouter clippedInput(clippedRoot);
    Check(clippedInput.TargetAt({45, 45}) == clippedChildPointer &&
              clippedInput.TargetAt({60, 60}) == nullptr,
          "ancestor clipping must exclude the invisible portion of a child from pointer hit testing");
    clippedRoot.SetClipContent(false);
    Check(clippedInput.TargetAt({60, 60}) == clippedChildPointer,
          "an unclipped child may receive pointer input outside its parent bounds");
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

void TestPublishedTextEffectsAreObservable() {
    Check(px::ui::RegisterBuiltinUITypes(),
          "text effect conformance requires built-in UI metadata");
    px::ui::GalgameUI ui;
    px::ui::DialoguePresentation presentation;
    presentation.text = "👩‍💻é終";
    Check(ui.ShowHUD(presentation), "text effect fixture should create the HUD");
    const auto findNamed = [](px::ui::Control* root, const std::string_view name,
                              const auto& self) -> px::ui::Control* {
        if (!root) return nullptr;
        if (root->Name() == name) return root;
        for (const auto& child : root->Children())
            if (auto* control = dynamic_cast<px::ui::Control*>(child.get()))
                if (auto* found = self(control, name, self)) return found;
        return nullptr;
    };
    auto* dialogue = findNamed(ui.Root(), "Dialogue", findNamed);
    Check(dialogue != nullptr, "HUD should expose its dialogue text target");
    px::animation::TrackBinding binding{
        .kind = px::animation::TargetKind::Text,
        .target = "Dialogue"};
    const auto apply = [&](const std::string& effect, const double progress) {
        binding.property = effect;
        return ui.ApplyAnimationProperty(binding, px::Variant(progress));
    };

    const float baseOpacity = dialogue->Opacity();
    Check(apply("fade", .5) && dialogue->Opacity() < baseOpacity,
          "published text fade must change rendered opacity");
    Check(apply("fade", 1.0) && dialogue->Opacity() == baseOpacity,
          "text fade must finish at the original opacity");
    const px::Rect baseOffsets = dialogue->Offsets();
    Check(apply("slide", .5) && dialogue->Offsets().x != baseOffsets.x,
          "published text slide must change its transform");
    Check(apply("slide", 1.0) && dialogue->Offsets().x == baseOffsets.x,
          "text slide must restore its authored transform at completion");
    const px::Vec2 baseScale = dialogue->Scale();
    Check(apply("pop", .5) && dialogue->Scale() != baseScale,
          "published text pop must change scale");
    Check(apply("pop", 1.0) && dialogue->Scale() == baseScale,
          "text pop must finish at authored scale");
    for (const char* effect : {"shake", "wave", "rainbow", "glitch"}) {
        const px::Rect beforeOffsets = dialogue->Offsets();
        const px::Color beforeTint = dialogue->Modulate();
        const px::Vec2 beforeScale = dialogue->Scale();
        const std::string executeMessage =
            std::string("published text effect must execute: ") + effect;
        Check(apply(effect, .43), executeMessage.c_str());
        const std::string visibleMessage =
            std::string("published text effect must visibly mutate the target: ") +
            effect;
        Check(dialogue->Offsets() != beforeOffsets || dialogue->Modulate() != beforeTint ||
                  dialogue->Scale() != beforeScale,
              visibleMessage.c_str());
        const std::string completionMessage =
            std::string("text effect should complete transactionally: ") + effect;
        Check(apply(effect, 1.0), completionMessage.c_str());
    }
    auto* label = dynamic_cast<px::ui::Label*>(dialogue);
    Check(label && apply("typewriter", .34) && label->Text() == "👩‍💻",
          "typewriter must reveal complete grapheme clusters");
    Check(apply("typewriter", 1.0) && label->Text() == presentation.text,
          "typewriter completion must restore the complete string");
}

void TestPublishedUiEffectsAreObservable() {
    Check(px::ui::RegisterBuiltinUITypes(),
          "UI effect conformance requires built-in UI metadata");
    px::ui::UIContext context;
    auto root = std::make_unique<px::ui::Control>("Root");
    auto target = std::make_unique<px::ui::Button>("Effect target");
    target->SetName("OfficialTarget");
    target->SetOffsets({80, 60, 240, 64});
    px::ui::Button* targetPointer = target.get();
    Check(root->AddChild(std::move(target)),
          "UI effect target should attach to its scene");
    Check(context.SetRoot(std::move(root)),
          "UI effect scene should install transactionally");
    if (!targetPointer) return;

    const px::Rect authoredOffsets = targetPointer->Offsets();
    const px::Vec2 authoredScale = targetPointer->Scale();
    const float authoredOpacity = targetPointer->Opacity();
    px::animation::TrackBinding binding{
        .kind = px::animation::TargetKind::UI,
        .target = "OfficialTarget"};
    for (const char* effect : {"panel-slide", "panel-fade", "panel-scale",
                               "modal-open", "modal-close", "button-hover",
                               "button-press"}) {
        binding.property = effect;
        const std::string execute =
            std::string("published UI preset must execute: ") + effect;
        Check(context.ApplyAnimationProperty(binding, px::Variant(0.43)),
              execute.c_str());
        const std::string visible =
            std::string("published UI preset must visibly mutate its target: ") +
            effect;
        Check(targetPointer->Offsets() != authoredOffsets ||
                  targetPointer->Scale() != authoredScale ||
                  targetPointer->Opacity() != authoredOpacity,
              visible.c_str());
        context.ResetAnimationPropertyOverrides("OfficialTarget");
        const std::string restored =
            std::string("UI preset reset must restore authored state: ") + effect;
        Check(targetPointer->Offsets() == authoredOffsets &&
                  targetPointer->Scale() == authoredScale &&
                  targetPointer->Opacity() == authoredOpacity,
              restored.c_str());
    }
}


}  // namespace

int main() {
    Run("VFS_DirectoryAndRegularFileReads", TestVfsRejectsDirectoriesAndReadsRegularFiles);
    Run("Typography_CjkRubyVerticalText", TestCjkRubyAndVerticalText);
    Run("Typography_ImmutableTextLayoutRanges", TestImmutableTextLayoutRanges);
    Run("UIFormat_TokensAndComponents", TestTypedFormatV4TokensAndComponents);
    Run("Designer_ImageAndTextRuntimeProperties", TestDesignerImageAndTextProperties);
    Run("Designer_CurrentRewriteBoundaries", TestDesignerRewriteContracts);
    Run("Controls_MetadataTransformsAndVideo", TestExpandedControlMetadataAndTransforms);
    Run("TextEffects_PublicPresetsAreObservable", TestPublishedTextEffectsAreObservable);
    Run("UIEffects_PublicPresetsAreObservable", TestPublishedUiEffectsAreObservable);
    if (g_failures == 0) std::cout << "PASS: runtime-resource integration\n";
    return g_failures == 0 ? 0 : 1;
}

