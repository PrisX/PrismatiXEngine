#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/BuiltinUiPackage.h"
#include "Engine/UI/Actions/BuiltInActionProvider.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/UiApplication.h"
#include "Engine/UI/VirtualizedView.h"
#include "Engine/UI/Widgets.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <utility>

namespace px::ui {
namespace {
Status EmitStatus(const Status& status) {
    for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
    return status;
}
Status GalgameFailure(std::string code,std::string message){diag::Diagnostic d{.severity=diag::Severity::Error,.code=std::move(code),.category="UI.Galgame",.message=std::move(message)};diag::Emit(d);return Status::Fail(std::move(d));}

class CardButton final : public Button {
public:
    using Button::Button;
    void SetImage(std::string value) { m_image = std::move(value); }
    void SetCaption(std::string title, std::string subtitle) {
        m_title = std::move(title); m_subtitle = std::move(subtitle);
        SetText(m_title);
        SetAccessibilityLabel(m_title);
        SetAccessibilityDescription(m_subtitle);
    }
    void CollectImageDependencies(std::vector<std::string>& paths) const override {
        if (!m_image.empty()) paths.push_back(m_image);
    }
protected:
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override {
        const auto& style = theme.Resolve(*this);
        const auto& box = !Enabled() ? style.disabled : Pressed() ? style.pressed :
            Focused() ? style.focused : Hovered() ? style.hover : style.normal;
        const auto area = LayoutRect();
        renderer.DrawRoundedRect(area, box.cornerRadius, box.background);
        if (!m_image.empty()) renderer.DrawImage(m_image, LayoutRect(), Enabled() ? 210 : 70);
        if (box.borderWidth > 0) renderer.DrawBorder(area, box.borderWidth, box.cornerRadius, box.border);
        const auto color = Enabled() ? style.text : style.textDisabled;
        const Rect title{area.x + 12, area.y + 6, std::max(0.0f, area.w - 24), 28};
        const Rect subtitle{title.x, area.y + 36, title.w, std::max(0.0f, area.h - 42)};
        renderer.PushClip(area);
        renderer.DrawTextInRect(m_title, title, style.font, std::min(style.fontSize, 22), color,
            graphics::HorizontalAlignment::Left, graphics::VerticalAlignment::Top, true);
        renderer.DrawTextInRect(m_subtitle, subtitle, style.font, std::min(style.fontSize, 16), color,
            graphics::HorizontalAlignment::Left, graphics::VerticalAlignment::Top, true);
        renderer.PopClip();
    }
private:
    std::string m_image;
    std::string m_title;
    std::string m_subtitle;
};

template<typename T>T* FindNamed(Control* root,std::string_view name){if(!root)return nullptr;if(root->Name()==name)return dynamic_cast<T*>(root);for(const auto& child:root->Children())if(auto* control=dynamic_cast<Control*>(child.get()))if(auto* found=FindNamed<T>(control,name))return found;return nullptr;}
bool UpdateChapterBanner(Control* root, std::string_view title) {
    auto* banner = FindNamed<Control>(root, "ChapterBanner");
    if (!banner) return false;
    constexpr std::string_view prefix = "CHAPTER ";
    const auto number = title.starts_with(prefix) ? title.substr(prefix.size()) : std::string_view{};
    const bool visible = !number.empty() && number.size() <= 4 &&
        std::ranges::all_of(number, [](char value) {
            return (value >= '0' && value <= '9') || value == '-';
        });
    banner->SetVisibility(visible ? Visibility::Visible : Visibility::Collapsed);
    if (auto* sprite = FindNamed<SpriteText>(banner, "ChapterNumber")) {
        sprite->SetText(visible ? std::string(number) : std::string{});
        return visible;
    }
    // Older authored HUDs use one hidden TextureRect per character and slot.
    for (std::size_t slot = 0; slot < 4; ++slot) {
        for (const char glyph : std::string_view("0123456789-")) {
            const std::string name = "ChapterGlyph" + std::to_string(slot) + "-" +
                (glyph == '-' ? "dash" : std::string(1, glyph));
            if (auto* image = FindNamed<TextureRect>(banner, name))
                image->SetVisibility(visible && slot < number.size() && number[slot] == glyph
                    ? Visibility::Visible : Visibility::Collapsed);
        }
    }
    return visible;
}
void SetPresentationText(Label* label, std::string value) {
    if (auto* rich = dynamic_cast<RichTextLabel*>(label))
        rich->SetMarkup(std::move(value));
    else if (label)
        label->SetText(std::move(value));
}
std::string_view ScreenId(const GalgameUI::Screen screen) {
    switch (screen) {
        case GalgameUI::Screen::Title: return "title";
        case GalgameUI::Screen::HUD: return "hud";
        case GalgameUI::Screen::Backlog: return "backlog";
        case GalgameUI::Screen::Save: return "save";
        case GalgameUI::Screen::Load: return "load";
        case GalgameUI::Screen::Gallery: return "gallery";
        case GalgameUI::Screen::Settings: return "settings";
        case GalgameUI::Screen::Video: return "video";
        case GalgameUI::Screen::Custom: return {};
    }
    return {};
}
std::optional<GalgameUI::Screen> ScreenFromId(const std::string_view id) {
    for (const auto screen : {GalgameUI::Screen::Title, GalgameUI::Screen::HUD,
                              GalgameUI::Screen::Backlog, GalgameUI::Screen::Save,
                              GalgameUI::Screen::Load, GalgameUI::Screen::Gallery,
                              GalgameUI::Screen::Settings, GalgameUI::Screen::Video})
        if (ScreenId(screen) == id) return screen;
    return std::nullopt;
}
}

class GalgameUI::ItemSource final : public IVirtualItemSource {
public:
    ItemSource(std::vector<GalgameItem> items, GalgameUI& owner) : m_items(std::move(items)), m_owner(owner) {}
    std::size_t Count() const override { return m_items.size(); }
    std::string StableKey(std::size_t index) const override { return m_items[index].key; }
    std::uint64_t Revision() const override { return m_revision; }
    Status Bind(std::size_t index, Control& item) override {
        if (index >= m_items.size()) {
            diag::Diagnostic d{.severity = diag::Severity::Error, .code = "PXUI2801", .category = "UI.Galgame",
                               .message = "Galgame list binding index is out of range"};
            diag::Emit(d); return Status::Fail(std::move(d));
        }
        auto* button = dynamic_cast<Button*>(&item);
        if (!button) {
            diag::Diagnostic d{.severity = diag::Severity::Error, .code = "PXUI2802", .category = "UI.Galgame",
                               .message = "Galgame virtual item factory must create Button controls"};
            diag::Emit(d); return Status::Fail(std::move(d));
        }
        const GalgameItem& value = m_items[index];
        button->SetText(value.title + (value.subtitle.empty() ? "" : "\n" + value.subtitle));
        button->SetEnabled(!value.disabled);
        if (auto* card = dynamic_cast<CardButton*>(button)) {
            card->SetImage(value.image);
            card->SetCaption(value.title, value.subtitle);
        }
        button->SetOnActivated([this, index] {
            if (index < m_items.size()) m_owner.Emit(m_items[index].command, m_items[index].argument);
        });
        return Status::Ok();
    }
private:
    std::vector<GalgameItem> m_items;
    GalgameUI& m_owner;
    std::uint64_t m_revision = 1;
};

GalgameUI::GalgameUI() {
    m_context.SetDiagnosticOverlayEnabled(true);
    (void)m_viewModel.Define("dialogue.speaker",Variant(std::string{}),true);
    (void)m_viewModel.Define("dialogue.text",Variant(std::string{}),true);
    (void)m_viewModel.Define("chapter.title",Variant(std::string{}),true);
    (void)m_viewModel.Define("music.title",Variant(std::string{}),true);
    // An authored slot template can bind into one typed collection. Keep the
    // flat fields for older authored UI documents until they are migrated.
    VariantArray initialSlots;
    initialSlots.reserve(12);
    for (std::size_t slot = 0; slot < 12; ++slot) {
        initialSlots.emplace_back(VariantObject{
            {"key", Variant(std::string("slot-") + std::to_string(slot))},
            {"number", Variant(slot < 9 ? std::string("0") + std::to_string(slot + 1) : std::to_string(slot + 1))},
            {"argument", Variant(std::to_string(slot))},
            {"image", Variant(std::string{})},
            {"occupied", Variant(false)},
            {"title", Variant(std::string{})},
            {"subtitle", Variant(std::string{})},
            {"enabled", Variant(false)},
        });
        const std::string prefix = "saveLoad.slot" + std::to_string(slot) + ".";
        (void)m_viewModel.Define(prefix + "image", Variant(std::string{}), true);
        (void)m_viewModel.Define(prefix + "title", Variant(std::string{}), true);
        (void)m_viewModel.Define(prefix + "subtitle", Variant(std::string{}), true);
        (void)m_viewModel.Define(prefix + "enabled", Variant(false), true);
    }
    (void)m_viewModel.Define("saveLoad.slots", Variant(std::move(initialSlots)), true);
    (void)m_viewModel.Define("gallery.items", Variant(VariantArray{}), true);
    for (const char* property : {"fullscreen", "windowed", "skipReadOnly", "skipAll"})
        (void)m_viewModel.Define(std::string("settings.") + property,
                                 Variant(false), true);
    for (const char* property : {"masterValue", "bgmValue", "seValue",
                                 "voiceValue", "textSpeedValue"})
        (void)m_viewModel.Define(std::string("settings.") + property,
                                 Variant(std::string{}), true);
    for (const char* property : {"visible", "save", "load", "overwrite"})
        (void)m_viewModel.Define(
            std::string("saveLoad.confirm.") + property, Variant(false), true);
    (void)m_viewModel.Define("title.confirm.visible", Variant(false), true);
    for (const char* property : {"visible", "reset", "exit"})
        (void)m_viewModel.Define(
            std::string("system.confirm.") + property, Variant(false), true);
    const std::vector<std::string> commands={
        "game.start", "game.continue", "load.open", "save.open",
        "gallery.open", "settings.open", "extra.open", "flowchart.open",
        "replay.open", "music.open", "title.open", "app.quit",
        "mode.auto", "mode.skip", "backlog.open", "hud.hide",
        "overlay.close"};
    for(const auto& command:commands)(void)RegisterActionCommand(command);
    (void)RegisterActionCommand("save.slot", "slot");
    (void)RegisterActionCommand("load.slot", "slot");
    (void)RegisterActionCommand("saveLoad.page", "argument");
    (void)RegisterActionCommand("cg.view", "resource");
    (void)RegisterActionCommand("saveLoad.confirm.accept");
    (void)RegisterActionCommand("saveLoad.confirm.cancel");
    (void)RegisterActionCommand("system.confirm.accept");
    (void)RegisterActionCommand("system.confirm.cancel");
    (void)m_context.Commands().Register("hud.toolbar.pin",[this](const Variant&){return ToggleHudToolbarPinned();});
    for (auto& entry : CreateBuiltinUiPackage()) {
        const auto screen = ScreenFromId(entry.route);
        if (!screen) continue;
        Template value;
        value.document = std::move(entry.document);
        value.sourcePath = "BuiltinUI/" + std::string(entry.route) + ".pxui";
        m_templates.emplace(std::string(entry.route), std::move(value));
    }
}

GalgameUI::~GalgameUI() {
    for (const auto& sourceId : m_hostActionSourceIds)
        (void)m_context.Actions().Catalog().RemoveSource(
            ActionOrigin::BuiltIn, sourceId);
}

Status GalgameUI::SetDefaultFont(std::string path) {
    auto style = m_context.ActiveTheme().StyleData();
    style.globalDefaults.properties["typography.font"] =
        StyleValue::Literal(Variant(std::move(path)));
    return m_context.ActiveTheme().SetStyleData(std::move(style));
}

Status GalgameUI::DefineUiData(std::string path, Variant value) {
    return m_viewModel.Define(std::move(path), std::move(value), true);
}

Status GalgameUI::UpdateUiData(const std::string_view path,
                               const Variant& value) {
    if (m_uiContractDocument.empty()) return m_viewModel.Write(path, value);
    const auto contract = nlohmann::json::parse(
        m_uiContractDocument, nullptr, false);
    bool declared = false;
    for (const auto& root : contract["roots"]) {
        const auto& name = root.get_ref<const std::string&>();
        if (path == name ||
            (path.size() > name.size() && path.substr(0, name.size()) == name &&
             (path[name.size()] == '.' || path[name.size()] == '['))) {
            declared = true;
            break;
        }
    }
    if (!declared) return m_viewModel.Write(path, value);

    // A scratch model has no subscribers. Replacing a collection here can
    // expose item fields that were absent when the contract was installed;
    // only the validated value is then sent to the live bindings.
    ObservableViewModel candidate;
    for (const auto& root : contract["roots"]) {
        const auto& name = root.get_ref<const std::string&>();
        auto current = m_viewModel.Read(name);
        if (!current) return Status::Fail(current.Diagnostics());
        const Status defined = candidate.Define(
            name, std::move(current.Value()), true);
        if (!defined) return defined;
    }
    const Status written = candidate.Write(path, value);
    if (!written) return written;
    const Status checked = ValidateUiContractForModel(
        m_uiContractDocument, candidate);
    if (!checked) return checked;
    return m_viewModel.Write(path, value);
}

Status GalgameUI::RegisterUiFormatter(Formatter formatter) {
    return m_context.Formatters().Register(std::move(formatter));
}

Status GalgameUI::ValidateUiContract(const std::string_view document) {
    const Status checked = ValidateUiContractForModel(document, m_viewModel);
    if (checked) m_uiContractDocument = document;
    return checked;
}

Status GalgameUI::ValidateUiContractForModel(
    const std::string_view document, const ObservableViewModel& model) const {
    const auto fail = [](std::string message, std::string detail = {}) {
        diag::Diagnostic diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXUI2823",
            .category = "UI.Galgame",
            .message = std::move(message),
            .details = std::move(detail)};
        return Status::Fail(std::move(diagnostic));
    };
    const auto contract = nlohmann::json::parse(document.begin(), document.end(),
                                                nullptr, false);
    if (!contract.is_object() || !contract.contains("format") ||
        contract["format"] != "PrismatiXUiContract" ||
        !contract.contains("schemaRevision") ||
        contract["schemaRevision"] != 1 ||
        !contract.contains("roots") || !contract["roots"].is_array() ||
        (contract.contains("values") && !contract["values"].is_object()) ||
        (!contract.contains("values") && !contract.contains("formatters")))
        return fail("Invalid UI host contract envelope");
    const auto declaredValues = contract.value("values", nlohmann::json::object());
    if (contract["roots"].size() > 256 || declaredValues.size() > 1024)
        return fail("UI host contract exceeds supported limits");
    const auto registeredRoots = model.EnumerateProperties();
    std::set<std::string> seenRoots;
    const auto expectedType = [](std::string_view name)
        -> std::optional<VariantType> {
        if (name == "null") return VariantType::Null;
        if (name == "boolean") return VariantType::Bool;
        if (name == "integer") return VariantType::Integer;
        if (name == "number") return VariantType::Number;
        if (name == "string") return VariantType::String;
        if (name == "array") return VariantType::Array;
        if (name == "object") return VariantType::Object;
        return std::nullopt;
    };
    for (const auto& root : contract["roots"]) {
        if (!root.is_string() || root.get_ref<const std::string&>().size() > 512)
            return fail("Invalid UI host contract root");
        const auto& path = root.get_ref<const std::string&>();
        if (!seenRoots.insert(path).second)
            return fail("Duplicate UI host contract root", path);
        if (!declaredValues.contains(path))
            return fail("UI host contract root has no type", path);
        if (!std::ranges::any_of(registeredRoots, [&](const PropertyPathInfo& entry) {
                return entry.path == path;
            }))
            return fail("UI host contract root is not registered", path);
    }
    std::size_t expandedPaths = 0;
    std::function<Status(const std::string&, const std::string&, VariantType)>
        checkValue = [&](const std::string& prefix, const std::string& suffix,
                         VariantType expected) -> Status {
        const auto wildcard = suffix.find("[]");
        if (wildcard == std::string::npos) {
            const std::string path = prefix + suffix;
            const auto actual = model.Describe(path);
            if (!actual || actual->type != expected)
                return fail("UI host value is missing or has the wrong type", path);
            return Status::Ok();
        }
        const std::string arrayPath = prefix + suffix.substr(0, wildcard);
        const auto array = model.Read(arrayPath);
        if (!array || array.Value().Type() != VariantType::Array)
            return fail("UI host contract expects an array", arrayPath);
        const auto* items = array.Value().AsArray();
        if (!items) return fail("UI host contract expects an array", arrayPath);
        for (std::size_t index = 0; index < items->size(); ++index) {
            if (++expandedPaths > 65536)
                return fail("UI host contract expands to too many values", arrayPath);
            const Status checked = checkValue(arrayPath + "[" +
                std::to_string(index) + "]", suffix.substr(wildcard + 2), expected);
            if (!checked) return checked;
        }
        return Status::Ok();
    };
    for (auto entry = declaredValues.begin(); entry != declaredValues.end(); ++entry) {
        if (!entry.value().is_string() || entry.key().size() > 512)
            return fail("Invalid UI host value declaration", entry.key());
        const auto type = expectedType(entry.value().get_ref<const std::string&>());
        if (!type) return fail("Unknown UI host value type", entry.key());
        const Status checked = checkValue({}, entry.key(), *type);
        if (!checked) return checked;
    }
    if (const auto formatters = contract.find("formatters");
        formatters != contract.end()) {
        if (!formatters->is_object() || formatters->size() > 128)
            return fail("Invalid UI host formatter declarations");
        for (auto entry = formatters->begin(); entry != formatters->end(); ++entry) {
            if (!entry.value().is_object() ||
                !entry.value().contains("input") ||
                !entry.value().contains("output") ||
                !entry.value()["input"].is_string() ||
                !entry.value()["output"].is_string())
                return fail("Invalid UI host formatter declaration", entry.key());
            const auto input = expectedType(
                entry.value()["input"].get_ref<const std::string&>());
            const auto output = expectedType(
                entry.value()["output"].get_ref<const std::string&>());
            const auto* registered = m_context.Formatters().Find(entry.key());
            if (!input || !output || !registered || registered->input != *input ||
                registered->output != *output)
                return fail("UI host formatter is missing or has the wrong signature",
                            entry.key());
        }
    }
    return Status::Ok();
}

Status GalgameUI::RegisterActionCommand(std::string command,
                                        std::string argumentKey) {
    if (command.empty())
        return GalgameFailure("PXUI2822", "UI action command cannot be empty");
    // CommandRegistry is the compatibility execution backend, while authored
    // .pxui triggers are validated and dispatched through ActionCatalog. Both
    // sides must know the id or a skinned button can install successfully but
    // fail with PXUIACTION2010 only when the player clicks it.
    auto& catalog = m_context.Actions().Catalog();
    if (!catalog.Contains(command)) {
        ActionDescriptor descriptor;
        descriptor.id = command;
        descriptor.label = command;
        descriptor.displayName = command;
        descriptor.category = "Game";
        descriptor.origin = ActionOrigin::BuiltIn;
        descriptor.sourceId = "GalgameUI.Dynamic";
        descriptor.providerId = "builtin";
        // Game routes commonly carry a string under a project-selected key.
        // Keep the catalog entry idempotent across multiple GalgameUI
        // instances and let each instance choose the key it forwards.
        descriptor.allowAdditionalArguments = true;
        const Status registered = catalog.Register(std::move(descriptor));
        if (!registered) return registered;
    }
    if (m_context.Commands().Contains(command)) return Status::Ok();
    const std::string registered = command;
    return m_context.Commands().Register(
        std::move(command),
        [this, registered, argumentKey = std::move(argumentKey)](
            const Variant& parameter) {
            std::string argument;
            if (const auto* values = parameter.AsObject()) {
                if (const auto found = values->find(argumentKey);
                    found != values->end()) {
                    if (const auto* value = found->second.TryGet<std::string>())
                        argument = *value;
                    else if (const auto* integer = found->second.TryGet<std::int64_t>())
                        argument = std::to_string(*integer);
                }
            }
            Emit(registered, std::move(argument));
            return Status::Ok();
        });
}

Status GalgameUI::RegisterHostAction(
    ActionDescriptor descriptor,
    std::function<Status(const ActionInvocation&)> handler) {
    if (!handler || descriptor.id.empty())
        return GalgameFailure("PXUI2823", "Host Action needs an id and handler");
    auto provider = std::dynamic_pointer_cast<BuiltInActionProvider>(
        m_context.Actions().FindProvider("builtin"));
    if (!provider)
        return GalgameFailure("PXUI2823", "Host Action provider is unavailable");
    descriptor.origin = ActionOrigin::BuiltIn;
    descriptor.providerId = "builtin";
    descriptor.sourceId = "GalgameUI.Host:" + descriptor.id;
    descriptor.allowAdditionalArguments = false;
    const std::string id = descriptor.id;
    const std::string sourceId = descriptor.sourceId;
    const Status declared = m_context.Actions().Catalog().Register(
        std::move(descriptor));
    if (!declared) return declared;
    const Status registered = provider->Register(id, std::move(handler));
    if (!registered) {
        (void)m_context.Actions().Catalog().RemoveSource(
            ActionOrigin::BuiltIn, sourceId);
        return registered;
    }
    m_hostActionSourceIds.push_back(sourceId);
    return Status::Ok();
}

Status GalgameUI::ValidateActionContract(const std::string_view document) {
    const auto fail = [](std::string message, std::string id = {}) {
        return GalgameFailure("PXUI2824", std::move(message) +
            (id.empty() ? std::string{} : ": " + id));
    };
    if (document.empty() || document.size() > 1024 * 1024)
        return fail("Host Action contract is empty or exceeds 1 MiB");
    const auto root = nlohmann::json::parse(document, nullptr, false);
    if (!root.is_object()) return fail("Invalid host Action contract");
    try {
        if (root.value("format", std::string{}) != "PrismatiXActionContract" ||
            root.value("schemaRevision", 0) != 1 ||
            !root.contains("actions") || !root["actions"].is_array() ||
            root["actions"].size() > 256)
            return fail("Invalid host Action contract envelope");
        const auto provider = std::dynamic_pointer_cast<BuiltInActionProvider>(
            m_context.Actions().FindProvider("builtin"));
        std::set<std::string> seen;
        const auto expectedType = [](const std::string_view type)
            -> std::optional<VariantType> {
            if (type == "boolean") return VariantType::Bool;
            if (type == "integer") return VariantType::Integer;
            if (type == "number") return VariantType::Number;
            if (type == "string") return VariantType::String;
            if (type == "resource") return VariantType::ResourceRef;
            return std::nullopt;
        };
        for (const auto& action : root["actions"]) {
            if (!action.is_object() || !action.contains("id") ||
                !action["id"].is_string() || !action.contains("parameters") ||
                !action["parameters"].is_array() ||
                action["parameters"].size() > 64 ||
                !action.contains("safety") || !action["safety"].is_object())
                return fail("Invalid host Action declaration");
            const auto id = action["id"].get<std::string>();
            if (id.empty() || !seen.insert(id).second)
                return fail("Duplicate or empty host Action", id);
            const auto* descriptor = m_context.Actions().Catalog().Find(id);
            if (!descriptor || descriptor->sourceId != "GalgameUI.Host:" + id ||
                descriptor->providerId != "builtin" || !provider ||
                !provider->HasHandler(id) || descriptor->allowAdditionalArguments ||
                descriptor->arguments.size() != action["parameters"].size())
                return fail("Host Action is missing or has the wrong signature", id);
            const auto& safety = action["safety"];
            if (safety.value("previewSafe", !descriptor->previewSafe) != descriptor->previewSafe ||
                safety.value("deterministic", !descriptor->deterministic) != descriptor->deterministic ||
                safety.value("seekSafe", !descriptor->seekSafe) != descriptor->seekSafe ||
                safety.value("rollbackSafe", !descriptor->rollbackSafe) != descriptor->rollbackSafe)
                return fail("Host Action safety contract does not match", id);
            std::set<std::string> parameters;
            for (const auto& parameter : action["parameters"]) {
                if (!parameter.is_object() || !parameter.contains("name") ||
                    !parameter["name"].is_string() || !parameter.contains("type") ||
                    !parameter["type"].is_string())
                    return fail("Invalid host Action parameter", id);
                const auto name = parameter["name"].get<std::string>();
                const auto type = expectedType(parameter["type"].get<std::string>());
                if (!type || !parameters.insert(name).second ||
                    std::none_of(descriptor->arguments.begin(),
                        descriptor->arguments.end(), [&](const auto& actual) {
                            return actual.name == name && actual.type == *type &&
                                actual.required == parameter.value("required", false);
                        }))
                    return fail("Host Action parameter does not match", id + "." + name);
            }
        }
    } catch (const nlohmann::json::exception&) {
        return fail("Host Action contract has invalid field types");
    }
    return Status::Ok();
}

UIRuntimeState GalgameUI::CaptureRuntimeState() const {
    UIRuntimeState state = m_context.CaptureRuntimeState();
    state.surfaceId = m_route;
    return state;
}

UIRuntimeState GalgameUI::CaptureGameplayRuntimeState() const {
    if (m_screen == Screen::HUD) return CaptureRuntimeState();
    if (m_lastGameplayState) return *m_lastGameplayState;
    UIRuntimeState empty;
    empty.surfaceId = "hud";
    return empty;
}

void GalgameUI::RememberGameplayCheckpoint(const Screen target) {
    if (target == m_screen || m_screen != Screen::HUD || !m_context.Root()) return;
    m_lastGameplayState = CaptureRuntimeState();
    m_lastGameplayState->surfaceId = "hud";
}

Status GalgameUI::ValidateRuntimeState(const UIRuntimeState& state) {
    if (state.surfaceId.empty() ||
        (!HasRouteTemplate(state.surfaceId) && !ScreenFromId(state.surfaceId)))
        return GalgameFailure("PXUI2817",
                              "UI checkpoint references an unknown surface: " +
                                  state.surfaceId);
    if (state.surfaceId == m_route) return m_context.ValidateRuntimeState(state);
    // Build the complete target document in an isolated UI runtime.  Action
    // providers are source-owned by the live ScriptHost, so validate those
    // handles against the live dispatcher and all topology/controller state
    // against the detached candidate.
    if (const Status actions =
            m_context.Actions().ValidateState(state.behavior.actions);
        !actions)
        return actions;
    GalgameUI candidate;
    candidate.m_templates = m_templates;
    candidate.m_studioAssetResolver = m_studioAssetResolver;
    candidate.m_studioComponentLoader = m_studioComponentLoader;
    candidate.m_studioMotionLoader = m_studioMotionLoader;
    if (const Status installed = candidate.InstallCheckpointRoute(state.surfaceId);
        !installed)
        return installed;
    UIRuntimeState candidateState = state;
    candidateState.behavior.actions.clear();
    return candidate.m_context.ValidateRuntimeState(candidateState);
}

Status GalgameUI::RestoreRuntimeState(const UIRuntimeState& state) {
    if (const Status valid = ValidateRuntimeState(state); !valid) return valid;
    const std::string previousRoute = m_route;
    const UIRuntimeState previousState = CaptureRuntimeState();
    if (state.surfaceId != m_route) {
        if (const Status installed = InstallCheckpointRoute(state.surfaceId); !installed)
            return installed;
    }
    if (const Status restored = m_context.RestoreRuntimeState(state); restored) {
        if (state.surfaceId == "hud") m_lastGameplayState = state;
        return restored;
    } else {
        Status rollback = Status::Ok();
        if (previousRoute != m_route) {
            rollback = InstallCheckpointRoute(previousRoute);
        }
        if (rollback) rollback = m_context.RestoreRuntimeState(previousState);
        if (!rollback)
            diag::Emit(diag::Diagnostic{
                .severity=diag::Severity::Fatal,.code="PXUI2820",
                .category="UI.Galgame",
                .message="UI surface restore failed and the previous surface could not be recovered"});
        return restored;
    }
}

Status GalgameUI::RegisterTemplate(Screen screen,std::string_view text,const std::string& sourcePath){
    const std::string_view route = ScreenId(screen);
    if (route.empty())
        return GalgameFailure("PXUI2821",
                              "Custom UI templates require an explicit route id");
    return RegisterRouteTemplate(std::string(route), text, sourcePath);
}

Status GalgameUI::RegisterRouteTemplate(std::string route,
                                        std::string_view text,
                                        const std::string& sourcePath){
    if (route.empty())
        return GalgameFailure("PXUI2821", "UI route id cannot be empty");
    auto canonical=sdk::ParseUi(text);
    if(canonical.Valid()){
        Template value;
        value.document=std::move(canonical.document);
        value.sourcePath=sourcePath;
        m_templates[std::move(route)]=std::move(value);
        return Status::Ok();
    }
    std::vector<diag::Diagnostic> diagnostics;
    for (const auto& error : canonical.diagnostics) {
        diag::Diagnostic diagnostic{
            .severity=diag::Severity::Error,.code=error.code,
            .category="UI.Galgame",.message=error.message};
        diagnostic.source.path=sourcePath;
        diag::Emit(diagnostic);
        diagnostics.push_back(std::move(diagnostic));
    }
    return Status::Fail(std::move(diagnostics));
}

bool GalgameUI::HasRouteTemplate(const std::string_view route) const {
    return m_templates.contains(std::string(route));
}

Result<std::vector<std::string>> GalgameUI::RouteImageDependencies(
    const std::string_view route) const {
    const auto found = m_templates.find(std::string(route));
    if (found == m_templates.end() || !found->second.document)
        return Result<std::vector<std::string>>::Failure(
            GalgameFailure("PXUI2804", "Requested UI route template is not registered: " +
                std::string(route)).Diagnostics());
    return CollectUiImageDependencies(*found->second.document,
        m_studioAssetResolver, m_studioComponentLoader, m_studioMotionLoader,
        found->second.sourcePath);
}

bool GalgameUI::HasTemplate(Screen screen)const{
    return HasRouteTemplate(ScreenId(screen));
}

Status GalgameUI::InstallTemplate(Screen screen){
    return InstallRouteTemplate(ScreenId(screen));
}

Status GalgameUI::InstallRouteTemplate(const std::string_view route){
    const auto it=m_templates.find(std::string(route));if(it==m_templates.end())return GalgameFailure("PXUI2804","Requested UI route template is not registered: "+std::string(route));
    const Screen screen = ScreenFromId(route).value_or(Screen::Custom);
    RememberGameplayCheckpoint(screen);
    ResetUiTimelineOverrides();
    if(it->second.document){
        m_bindings.clear();
        UiApplication application(m_context);
        auto applied=application.ApplyDocument(
            *it->second.document,
            {.sourcePath=it->second.sourcePath,
             .resolveAsset=m_studioAssetResolver,
             .loadComponent=m_studioComponentLoader,
             .loadMotion=m_studioMotionLoader,
             .viewModel=&m_viewModel,
             .previewSafeMode=false,
             .diagnosticOverlay=false});
        if(!applied){
            for(const auto& diagnostic:applied.Diagnostics())diag::Emit(diagnostic);
            return Status::Fail(applied.Diagnostics());
        }
        if(screen!=Screen::HUD){
            m_speaker=m_dialogue=m_nvlText=m_mode=m_chapterNotice=
                m_musicNotice=nullptr;
            m_choices=nullptr;
            m_presentedChoices.clear();
            m_noticePanel=nullptr;
        }
        m_screen=screen;
        m_route=route;
    }else{
        return GalgameFailure("PXUI2805","Registered UI template has no document");
    }
    if(screen==Screen::HUD){m_speaker=FindNamed<Label>(m_context.Root(),"Speaker");m_dialogue=FindNamed<Label>(m_context.Root(),"Dialogue");m_nvlText=FindNamed<Label>(m_context.Root(),"NVLText");m_choices=FindNamed<VBoxContainer>(m_context.Root(),"Choices");m_presentedChoices.clear();m_mode=FindNamed<Label>(m_context.Root(),"ModeState");m_noticePanel=FindNamed<EdgeRevealContainer>(m_context.Root(),"NoticePanel");m_chapterNotice=FindNamed<Label>(m_context.Root(),"ChapterNotice");m_musicNotice=FindNamed<Label>(m_context.Root(),"MusicNotice");m_lastChapterTitle.clear();m_lastMusicTitle.clear();m_activeDialogueEffect.clear();m_dialogueEffectFinished=false;if(m_dialogue)m_dialogueBaseFontSize=m_dialogue->FontSize()>0?m_dialogue->FontSize():30;}
    return Status::Ok();
}

Status GalgameUI::InstallCheckpointSurface(const Screen screen) {
    if (HasTemplate(screen)) return InstallTemplate(screen);
    switch (screen) {
        case Screen::Title: return ShowTitle();
        case Screen::HUD: return ShowHUD({});
        case Screen::Backlog: return ShowBacklog({});
        case Screen::Save: return ShowSaveLoad(true, {});
        case Screen::Load: return ShowSaveLoad(false, {});
        case Screen::Gallery: return ShowGallery({});
        case Screen::Settings: return ShowSettings({});
        case Screen::Video: return ShowVideoOverlay(false);
        case Screen::Custom: break;
    }
    return GalgameFailure("PXUI2818", "UI checkpoint surface is unsupported");
}

Status GalgameUI::InstallCheckpointRoute(const std::string_view route) {
    if (HasRouteTemplate(route)) return InstallRouteTemplate(route);
    const auto screen = ScreenFromId(route);
    return screen ? InstallCheckpointSurface(*screen)
                  : GalgameFailure("PXUI2818",
                                   "UI checkpoint route is unsupported: " +
                                       std::string(route));
}

Status GalgameUI::ShowRoute(const std::string_view route) {
    return InstallCheckpointRoute(route);
}
void GalgameUI::Emit(std::string command, std::string argument) { m_pendingActions.push_back({std::move(command),std::move(argument)}); }

Status GalgameUI::Add(Control& parent, std::unique_ptr<Control> child) {
    const Status status = parent.AddChild(std::move(child)); return status ? status : EmitStatus(status);
}

std::unique_ptr<Control> GalgameUI::MakeMenuButton(std::string text, std::string command, std::string argument) {
    auto button = std::make_unique<Button>(std::move(text));
    button->SetOnActivated([this, command = std::move(command), argument = std::move(argument)] { Emit(command, argument); });
    return button;
}

Status GalgameUI::Install(std::unique_ptr<Control> root, Screen screen,
                          std::string route) {
    RememberGameplayCheckpoint(screen);
    ResetUiTimelineOverrides();
    m_bindings.clear();
    if (screen != Screen::HUD) {
        m_speaker = m_dialogue = m_nvlText = m_mode = nullptr;
        m_choices = nullptr;
        m_presentedChoices.clear();
    }
    const Status status = m_context.SetRoot(std::move(root)); if (!status) return status;
    m_screen = screen;
    m_route = route.empty() ? std::string(ScreenId(screen)) : std::move(route);
    return Status::Ok();
}

Status GalgameUI::ApplyAnimationProperty(
    const animation::TrackBinding& binding, const Variant& value) {
    return m_context.ApplyAnimationProperty(binding, value);
}

Status GalgameUI::ValidateAnimationProperty(
    const animation::TrackBinding& binding, const Variant& value,
    const UIRuntimeState& state) {
    if (state.surfaceId.empty() ||
        (!HasRouteTemplate(state.surfaceId) && !ScreenFromId(state.surfaceId)))
        return GalgameFailure("PXUI2817",
                              "Animation checkpoint references an unknown UI surface");
    GalgameUI candidate;
    candidate.m_templates = m_templates;
    candidate.m_studioAssetResolver = m_studioAssetResolver;
    candidate.m_studioComponentLoader = m_studioComponentLoader;
    candidate.m_studioMotionLoader = m_studioMotionLoader;
    if (const Status installed = candidate.InstallCheckpointRoute(state.surfaceId);
        !installed)
        return installed;
    return candidate.ApplyAnimationProperty(binding, value);
}

Status GalgameUI::ShowTitle() {
    return InstallTemplate(Screen::Title);
}

Status GalgameUI::ActivateUiControl(const std::string_view nodeId) {
    const auto id = Uuid::Parse(nodeId);
    if (!id)
        return GalgameFailure("PXUI2812",
                              "UI document activation requires a canonical node UUID");
    auto* button = m_context.Root()
                       ? dynamic_cast<Button*>(m_context.Root()->Find(*id))
                       : nullptr;
    if (!button)
        return GalgameFailure(
            "PXUI2813",
            "UI document activation target is not a Button-compatible control");
    button->Activate();
    return Status::Ok();
}

Status GalgameUI::SetUiControlVisibility(const std::string_view nodeId,
                                             const bool visible) {
    const auto id = Uuid::Parse(nodeId);
    if (!id)
        return GalgameFailure(
            "PXUI2816",
            "UI document visibility requires a canonical node UUID");
    auto* control = m_context.Root()
                        ? dynamic_cast<Control*>(m_context.Root()->Find(*id))
                        : nullptr;
    if (!control)
        return GalgameFailure(
            "PXUI2817",
            "UI document visibility target is not present in the active scene");
    m_timelineVisibilityBase.try_emplace(std::string(nodeId),
                                         control->GetVisibility());
    control->SetVisibility(visible ? Visibility::Visible : Visibility::Hidden);
    return Status::Ok();
}

Status GalgameUI::PreviewUiAnimation(const std::string_view clipId,
                                         const float time,
                                         const bool playing) {
    const auto id = Uuid::Parse(clipId);
    if (!id)
        return GalgameFailure(
            "PXUI2818",
            "UI document animation requires a canonical clip UUID");
    return m_context.PreviewAnimation(*id, time, playing);
}

Status GalgameUI::StopUiAnimation(const bool restoreDesignState) {
    return m_context.StopAnimation(restoreDesignState);
}

void GalgameUI::ResetUiTimelineOverrides() {
    m_context.ResetAnimationPropertyOverrides();
    for (const auto& [nodeId, visibility] : m_timelineVisibilityBase) {
        const auto id = Uuid::Parse(nodeId);
        auto* control = id && m_context.Root()
                            ? dynamic_cast<Control*>(m_context.Root()->Find(*id))
                            : nullptr;
        if (control) control->SetVisibility(visibility);
    }
    m_timelineVisibilityBase.clear();
}

Status GalgameUI::ToggleHudToolbarPinned() {
    auto* toolbar = FindNamed<EdgeRevealContainer>(m_context.Root(), "EdgeToolbar");
    if (!toolbar)
        return GalgameFailure(
            "PXUI2814",
            "HUD toolbar pinning requires an EdgeToolbar control on the active screen");
    toolbar->TogglePinned();
    return Status::Ok();
}

Status GalgameUI::ActivateChoice(const std::size_t index) {
    if (!m_choices || index >= m_presentedChoices.size() ||
        index >= m_choices->Children().size())
        return GalgameFailure("PXUI2815",
                              "Choice activation index is outside the active HUD choices");
    auto* button = dynamic_cast<Button*>(m_choices->Children()[index].get());
    if (!button)
        return GalgameFailure("PXUI2816",
                              "Generated HUD choice is not Button-compatible");
    button->Activate();
    return Status::Ok();
}

Status GalgameUI::ShowHUD(const DialoguePresentation& p) {
    const Status status=InstallTemplate(Screen::HUD);if(!status)return status;
    return RefreshHUD(p);
}

Status GalgameUI::RefreshHUD(const DialoguePresentation& p) {
    if (m_screen != Screen::HUD || !m_context.Root()) return ShowHUD(p);
    if (m_speaker) m_speaker->SetText(p.speaker);
    if (m_dialogue) {
        SetPresentationText(m_dialogue, p.text);
        const int requested = static_cast<int>(std::lround(
            m_dialogueBaseFontSize * std::clamp(p.textScale, .75f, 2.0f)));
        const Rect bounds = m_dialogue->LayoutRect();
        m_dialogue->SetFontSize(m_dialogue->FontSizeToFit(
            p.fullText.empty() ? p.text : p.fullText, requested,
            bounds.w, bounds.h));
        const std::string desiredEffect = p.reducedMotion ? std::string{} : p.effect;
        if (desiredEffect != m_activeDialogueEffect) {
            if (!m_activeDialogueEffect.empty())
                m_context.ResetAnimationPropertyOverrides("Dialogue");
            m_activeDialogueEffect = desiredEffect;
            m_dialogueEffectFinished = false;
        }
        const bool looping = desiredEffect == "wave" || desiredEffect == "rainbow";
        if (!desiredEffect.empty() &&
            (looping || !m_dialogueEffectFinished)) {
            const float elapsed = std::max(0.0f, p.effectProgress) / 0.6f;
            const float progress = looping ? std::fmod(elapsed, 1.0f)
                                           : std::min(elapsed, 1.0f);
            animation::TrackBinding binding{
                .kind = animation::TargetKind::Text,
                .target = "Dialogue",
                .property = desiredEffect};
            if (const Status applied =
                    m_context.ApplyAnimationProperty(binding, Variant(progress));
                !applied)
                return applied;
            m_dialogueEffectFinished = !looping && progress >= 1.0f;
        }
    }
    (void)m_viewModel.Write("dialogue.speaker",Variant(p.speaker));(void)m_viewModel.Write("dialogue.text",Variant(p.text));
    (void)m_viewModel.Write("chapter.title",Variant(p.chapterTitle));(void)m_viewModel.Write("music.title",Variant(p.musicTitle));
    const bool chapterChanged=p.chapterTitle!=m_lastChapterTitle;
    const bool musicChanged=p.musicTitle!=m_lastMusicTitle;
    const bool chapterBannerVisible = chapterChanged &&
        UpdateChapterBanner(m_context.Root(), p.chapterTitle);
    if(m_noticePanel&&(chapterChanged||musicChanged)){
        const bool showChapter=chapterChanged&&!chapterBannerVisible&&!p.chapterTitle.empty();
        const bool showMusic=!showChapter&&musicChanged&&!p.musicTitle.empty();
        // Authored HUDs may deliberately leave notice labels empty and feed
        // them from the runtime presentation model.  View-model writes only
        // update controls that carry an explicit binding, so also update the
        // conventional named controls just as we already do for dialogue.
        if(m_chapterNotice)SetPresentationText(m_chapterNotice,p.chapterTitle);
        if(m_musicNotice)SetPresentationText(m_musicNotice,p.musicTitle);
        if(m_chapterNotice)m_chapterNotice->SetVisibility(showChapter?Visibility::Visible:Visibility::Collapsed);
        if(m_musicNotice)m_musicNotice->SetVisibility(showMusic?Visibility::Visible:Visibility::Collapsed);
        if(showChapter||showMusic)m_noticePanel->RevealFor(5.0f);
    }
    m_lastChapterTitle=p.chapterTitle;m_lastMusicTitle=p.musicTitle;
    if (m_nvlText) {
        std::string text; for (const auto& line : p.nvlLines) text += line + "[br][br]"; SetPresentationText(m_nvlText, std::move(text));
        if (auto* panel = dynamic_cast<Control*>(m_nvlText->Parent())) panel->SetVisibility(p.nvlMode ? Visibility::Visible : Visibility::Collapsed);
    }
    if (m_dialogue) if (auto* panel = dynamic_cast<Control*>(m_dialogue->Parent())) panel->SetVisibility(p.nvlMode ? Visibility::Collapsed : Visibility::Visible);
    if (m_mode) m_mode->SetText(std::string(p.autoMode ? "● AUTO  " : "") + (p.skipMode ? "▶▶ SKIP" : ""));
    if (auto* toolbar = FindNamed<EdgeRevealContainer>(m_context.Root(), "EdgeToolbar"))
        toolbar->SetReducedMotion(p.reducedMotion);
    if (m_choices && p.choices != m_presentedChoices) {
        for (std::size_t i = 0; i < p.choices.size(); ++i) {
            Button* button = i < m_choices->Children().size()
                                 ? dynamic_cast<Button*>(m_choices->Children()[i].get())
                                 : nullptr;
            if (!button) {
                if (auto status = Add(*m_choices, MakeMenuButton(
                        p.choices[i], "choice.select", std::to_string(i)));
                    !status)
                    return status;
                button = dynamic_cast<Button*>(m_choices->Children().back().get());
            } else {
                button->SetText(p.choices[i]);
                button->SetOnActivated([this, i] {
                    Emit("choice.select", std::to_string(i));
                });
            }
            if (button) button->SetVisibility(Visibility::Visible);
        }
        for (std::size_t i = p.choices.size(); i < m_choices->Children().size(); ++i)
            if (auto* button = dynamic_cast<Button*>(m_choices->Children()[i].get()))
                button->SetVisibility(Visibility::Collapsed);
        m_presentedChoices = p.choices;
        m_choices->InvalidateLayout();
    }
    return Status::Ok();
}

Status GalgameUI::ShowBacklog(std::vector<GalgameItem> entries) {
    const Status installed=InstallTemplate(Screen::Backlog);if(!installed)return installed;auto* list=FindNamed<ListView>(m_context.Root(),"Entries");if(!list)return GalgameFailure("PXUI2805","Backlog template requires a ListView named Entries");m_items=std::make_shared<ItemSource>(std::move(entries),*this);return list->SetSource(m_items,[]{return std::make_unique<Button>();});
}

Status GalgameUI::ShowSaveLoad(bool saveMode, std::vector<GalgameItem> slots) {
    const Screen target=saveMode?Screen::Save:Screen::Load;
    (void)SetSaveLoadConfirmation(SaveLoadPrompt::None);
    VariantArray slotValues;
    slotValues.reserve(slots.size());
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const GalgameItem* item = &slots[index];
        slotValues.emplace_back(VariantObject{
            {"key", Variant(item->argument)},
            {"number", Variant(item->argument)},
            {"argument", Variant(item->argument)},
            {"image", Variant(item ? item->image : std::string{})},
            {"occupied", Variant(item && !item->title.empty())},
            {"title", Variant(item ? item->title : std::string{})},
            {"subtitle", Variant(item ? item->subtitle : std::string{})},
            {"enabled", Variant(item && !item->disabled)},
        });
        const std::string prefix = "saveLoad.slot" + std::to_string(index) + ".";
        (void)m_viewModel.Write(prefix + "image", Variant(item ? item->image : std::string{}));
        (void)m_viewModel.Write(prefix + "title", Variant(item ? item->title : std::string{}));
        (void)m_viewModel.Write(prefix + "subtitle", Variant(item ? item->subtitle : std::string{}));
        (void)m_viewModel.Write(prefix + "enabled", Variant(item && !item->disabled));
    }
    const Status populated = m_viewModel.Write("saveLoad.slots", Variant(std::move(slotValues)));
    if (!populated) return populated;
    const Status installed = InstallTemplate(target);
    if (!installed) return installed;
    if (FindNamed<Button>(m_context.Root(), "Slot0Button")) {
        for (std::size_t index = 1; index < 12; ++index)
            if (!FindNamed<Button>(m_context.Root(), "Slot" + std::to_string(index) + "Button"))
                return GalgameFailure("PXUI2806", "Authored save/load template is missing a slot button");
        m_items.reset();
        return Status::Ok();
    }
    auto* grid = FindNamed<GridView>(m_context.Root(), "Slots");
    if (!grid) return GalgameFailure("PXUI2806", "SaveLoad template requires authored Slot0Button..Slot11Button or a GridView named Slots");
    if (grid->HasSource()) {
        m_items.reset();
        return Status::Ok();
    }
    for (auto& item : slots) {
        std::string prefix = item.argument == "-1" ? "AUTO" : std::string{};
        for (std::size_t index = 0; index < 12 && prefix.empty(); ++index)
            if (item.argument == std::to_string(index))
                prefix = "#" + std::to_string(index + 1);
        if (!prefix.empty()) item.title = prefix + "  " + (item.title.empty() ? "—" : item.title);
    }
    m_items = std::make_shared<ItemSource>(std::move(slots), *this);
    return grid->SetSource(m_items, [] { return std::make_unique<CardButton>(); });
}

bool GalgameUI::SupportsSaveLoadConfirmation() const {
    return (m_screen == Screen::Save || m_screen == Screen::Load) &&
           FindNamed<Control>(m_context.Root(), "SaveLoadConfirm") != nullptr;
}

Status GalgameUI::SetSaveLoadConfirmation(const SaveLoadPrompt prompt) {
    for (const auto& [name, enabled] : {
             std::pair{"visible", prompt != SaveLoadPrompt::None},
             std::pair{"save", prompt == SaveLoadPrompt::Save},
             std::pair{"load", prompt == SaveLoadPrompt::Load},
             std::pair{"overwrite", prompt == SaveLoadPrompt::Overwrite}}) {
        const Status written = m_viewModel.Write(
            std::string("saveLoad.confirm.") + name, Variant(enabled));
        if (!written) return written;
    }
    return Status::Ok();
}

bool GalgameUI::SupportsTitleConfirmation() const {
    return FindNamed<Control>(m_context.Root(), "TitleConfirm") != nullptr;
}

Status GalgameUI::SetTitleConfirmation(const bool visible) {
    return m_viewModel.Write("title.confirm.visible", Variant(visible));
}

bool GalgameUI::SupportsSystemConfirmation() const {
    return FindNamed<Control>(m_context.Root(), "SystemConfirm") != nullptr;
}

Status GalgameUI::SetSystemConfirmation(const SystemPrompt prompt) {
    for (const auto& [name, enabled] : {
             std::pair{"visible", prompt != SystemPrompt::None},
             std::pair{"reset", prompt == SystemPrompt::Reset},
             std::pair{"exit", prompt == SystemPrompt::Exit}}) {
        const Status written = m_viewModel.Write(
            std::string("system.confirm.") + name, Variant(enabled));
        if (!written) return written;
    }
    return Status::Ok();
}

Status GalgameUI::ShowGallery(std::vector<GalgameItem> entries) {
    VariantArray authoredItems;
    authoredItems.reserve(entries.size());
    for (const auto& entry : entries) {
        authoredItems.emplace_back(VariantObject{
            {"key", Variant(entry.key)},
            {"title", Variant(entry.title)},
            {"image", Variant(entry.image)},
            {"enabled", Variant(!entry.disabled)},
            {"locked", Variant(entry.disabled)},
        });
    }
    if (const Status populated = m_viewModel.Write("gallery.items", Variant(std::move(authoredItems)));
        !populated) return populated;
    const Status installed = InstallTemplate(Screen::Gallery);
    if (!installed) return installed;
    auto* grid = FindNamed<GridView>(m_context.Root(), "Items");
    if (!grid) return GalgameFailure("PXUI2807", "Gallery template requires a GridView named Items");
    // ApplyDocument installs authored itemSource grids before returning. A
    // pre-existing source is authoritative here; replacing it with the
    // compatibility CardButton source discards the authored CG cell subtree.
    if (grid->HasSource()) {
        m_items.reset();
        return Status::Ok();
    }
    m_items = std::make_shared<ItemSource>(std::move(entries), *this);
    return grid->SetSource(m_items, [] { return std::make_unique<CardButton>(); });
}

Status GalgameUI::ShowSettings(const SettingsPresentation& s) {
    (void)m_viewModel.Write("settings.fullscreen", Variant(s.fullscreen));
    (void)m_viewModel.Write("settings.windowed", Variant(!s.fullscreen));
    (void)m_viewModel.Write("settings.skipReadOnly", Variant(s.skipReadOnly));
    (void)m_viewModel.Write("settings.skipAll", Variant(!s.skipReadOnly));
    const auto percent=[](double value,double maximum){
        return std::to_string(static_cast<int>(value*100.0/maximum+0.5));
    };
    (void)m_viewModel.Write("settings.masterValue", Variant(percent(s.master,128)));
    (void)m_viewModel.Write("settings.bgmValue", Variant(percent(s.bgm,128)));
    (void)m_viewModel.Write("settings.seValue", Variant(percent(s.se,128)));
    (void)m_viewModel.Write("settings.voiceValue", Variant(percent(s.voice,128)));
    (void)m_viewModel.Write("settings.textSpeedValue", Variant(percent(s.textSpeedMs,120)));
    const Status installed=InstallTemplate(Screen::Settings);if(!installed)return installed;
    auto slider=[this,percent](const char* name,double value,double maximum,const char* command,const char* label=nullptr){if(auto* control=FindNamed<Slider>(m_context.Root(),name)){control->SetRange(0,maximum,maximum<=128?4:1);control->SetValue(value);control->SetOnChanged([this,command,label,maximum,percent](double changed){if(label)(void)m_viewModel.Write(label,Variant(percent(changed,maximum)));Emit(command,std::to_string(static_cast<int>(changed)));});}};
    slider("Master",s.master,128,"set.master.value","settings.masterValue");slider("BGM",s.bgm,128,"set.bgm.value","settings.bgmValue");slider("SE",s.se,128,"set.se.value","settings.seValue");slider("Voice",s.voice,128,"set.voice.value","settings.voiceValue");slider("TextSpeed",s.textSpeedMs,120,"set.speed.value","settings.textSpeedValue");
    if(auto* skip=FindNamed<CheckBox>(m_context.Root(),"SkipRead")){skip->SetChecked(s.skipReadOnly);skip->SetOnToggled([this](bool value){Emit("set.skipread.value",value?"true":"false");});}
    if(auto* full=FindNamed<CheckBox>(m_context.Root(),"Fullscreen")){full->SetChecked(s.fullscreen);full->SetOnToggled([this](bool value){Emit("set.fullscreen.value",value?"true":"false");});}
    slider("TextScale",s.textScale*100.0,200,"set.textscale.value");
    if(auto* value=FindNamed<CheckBox>(m_context.Root(),"HighContrast")){value->SetChecked(s.highContrast);value->SetOnToggled([this](bool changed){Emit("set.highcontrast.value",changed?"true":"false");});}
    if(auto* value=FindNamed<CheckBox>(m_context.Root(),"ReducedMotion")){value->SetChecked(s.reducedMotion);value->SetOnToggled([this](bool changed){Emit("set.reducedmotion.value",changed?"true":"false");});}
    if(auto* value=FindNamed<CheckBox>(m_context.Root(),"SelfVoicing")){value->SetChecked(s.selfVoicing);value->SetOnToggled([this](bool changed){Emit("set.selfvoicing.value",changed?"true":"false");});}
    if(auto* value=FindNamed<OptionButton>(m_context.Root(),"Language")){value->SetOptions(s.languages);const auto selected=std::find(s.languages.begin(),s.languages.end(),s.language);value->SetSelected(selected==s.languages.end()?0:static_cast<int>(std::distance(s.languages.begin(),selected)));value->SetOnActivated([this,value](){if(value->Selected()>=0&&static_cast<std::size_t>(value->Selected())<value->Options().size())Emit("set.language.value",value->Options()[static_cast<std::size_t>(value->Selected())]);});}
    return Status::Ok();
}

Status GalgameUI::ShowVideoOverlay(bool skippable) {
    const Status installed = InstallTemplate(Screen::Video);
    if (!installed) return installed;
    auto* hint = FindNamed<Label>(m_context.Root(), "SkipHint");
    if (!hint) return GalgameFailure("PXUI2808", "Video template requires a Label named SkipHint");
    hint->SetText(skippable ? "點擊跳過 ▶" : "");
    return Status::Ok();
}

bool GalgameUI::Update(const Input& input, int width, int height,float deltaSeconds) { const bool consumed=m_context.Update(input,width,height,deltaSeconds);auto actions=std::move(m_pendingActions);m_pendingActions.clear();for(const auto& action:actions)if(m_sink)m_sink(action);return consumed; }
void GalgameUI::Render(graphics::Renderer2D& renderer) { m_context.Render(renderer); }

}  // namespace px::ui
