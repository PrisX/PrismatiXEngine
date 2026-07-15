#include "Engine/UI/Actions/BuiltInActionProvider.h"

#include "Engine/Diagnostics/Diagnostic.h"

namespace px::ui {
namespace {

diag::Diagnostic BuiltInError(std::string code, std::string message) {
    return diag::Diagnostic{.severity = diag::Severity::Error,
                            .code = std::move(code),
                            .category = "UI.Action",
                            .message = std::move(message)};
}

ActionArgumentDescriptor Argument(std::string name, std::string label, const VariantType type,
                                  const ActionEditorHint hint = ActionEditorHint::Default,
                                  const bool required = true) {
    ActionArgumentDescriptor argument;
    argument.name = std::move(name);
    argument.displayName = std::move(label);
    argument.type = type;
    argument.required = required;
    argument.editorHint = hint;
    return argument;
}

ActionDescriptor Descriptor(std::string id, std::string label, std::string category,
                            std::vector<ActionArgumentDescriptor> arguments = {},
                            const bool destructive = false) {
    ActionDescriptor descriptor;
    descriptor.id = std::move(id);
    descriptor.label = label;
    descriptor.displayName = std::move(label);
    descriptor.category = std::move(category);
    descriptor.origin = ActionOrigin::BuiltIn;
    descriptor.sourceId = "PrismatiX";
    descriptor.providerId = "builtin";
    descriptor.arguments = std::move(arguments);
    descriptor.destructiveInPreview = destructive;
    return descriptor;
}

}  // namespace

bool BuiltInActionProvider::CanInvoke(const std::string_view action) const {
    return m_handlers.contains(std::string(action)) || static_cast<bool>(m_fallback);
}

Status BuiltInActionProvider::Invoke(const ActionInvocation& invocation) {
    if (const auto found = m_handlers.find(invocation.action); found != m_handlers.end())
        return found->second(invocation);
    if (m_fallback) return m_fallback(invocation);
    return Status::Fail(BuiltInError("PXUIACTION2030",
        "Built-in action has no runtime handler: " + invocation.action));
}

Status BuiltInActionProvider::Register(std::string action, Handler handler) {
    if (action.empty() || !handler)
        return Status::Fail(BuiltInError("PXUIACTION2031", "Invalid built-in action handler"));
    if (m_handlers.contains(action))
        return Status::Fail(BuiltInError("PXUIACTION2032", "Duplicate built-in action handler: " + action));
    m_handlers.emplace(std::move(action), std::move(handler));
    return Status::Ok();
}

std::vector<ActionDescriptor> BuiltInActionDescriptors() {
    std::vector<ActionDescriptor> descriptors;
    const auto add = [&](ActionDescriptor descriptor) { descriptors.push_back(std::move(descriptor)); };
    add(Descriptor("game.start", "Start Game", "Game"));
    add(Descriptor("app.quit", "Quit", "Application", {}, true));
    add(Descriptor("overlay.close", "Close / Back", "Navigation"));
    add(Descriptor("load.open", "Open Load", "Navigation"));
    add(Descriptor("save.open", "Open Save", "Navigation"));
    add(Descriptor("gallery.open", "Open Gallery", "Navigation"));
    add(Descriptor("settings.open", "Open Settings", "Navigation"));
    add(Descriptor("backlog.open", "Open Backlog", "Navigation"));
    add(Descriptor("choice.select", "Select Choice", "Scenario",
                   {Argument("index", "Choice Index", VariantType::Integer)}));
    add(Descriptor("load.slot", "Load Slot", "Save",
                   {Argument("slot", "Slot", VariantType::Integer)}));
    add(Descriptor("save.slot", "Save Slot", "Save",
                   {Argument("slot", "Slot", VariantType::Integer)}));
    add(Descriptor("cg.view", "View CG", "Gallery",
                   {Argument("resource", "CG", VariantType::String, ActionEditorHint::Resource)}));
    add(Descriptor("backlog.voice", "Replay Voice", "Backlog",
                   {Argument("index", "Backlog Index", VariantType::Integer)}));
    add(Descriptor("backlog.rollback", "Rollback", "Backlog",
                   {Argument("index", "Backlog Index", VariantType::Integer)}));
    add(Descriptor("mode.auto", "Toggle Auto", "Game"));
    add(Descriptor("mode.skip", "Toggle Skip", "Game"));
    add(Descriptor("set.skipread.toggle", "Toggle Skip Read", "Settings"));
    add(Descriptor("set.fullscreen.toggle", "Toggle Fullscreen", "Settings"));
    add(Descriptor("hud.toolbar.pin", "Pin / unpin HUD toolbar", "HUD"));
    add(Descriptor("animation.trigger", "Set Animation Trigger", "Animation",
                   {Argument("parameter", "Parameter", VariantType::String)}));
    add(Descriptor("animation.bool", "Set Animation Bool", "Animation",
                   {Argument("parameter", "Parameter", VariantType::String),
                    Argument("value", "Value", VariantType::Bool)}));
    add(Descriptor("animation.number", "Set Animation Number", "Animation",
                   {Argument("parameter", "Parameter", VariantType::String),
                    Argument("value", "Value", VariantType::Number)}));
    add(Descriptor("animation.travel", "Travel Animation State", "Animation",
                   {Argument("state", "State", VariantType::String),
                    Argument("duration", "Transition Duration", VariantType::Number)}));
    for (const char* bus : {"bgm", "se", "voice", "speed"}) {
        const std::string name(bus);
        add(Descriptor("set." + name + ".up", "Increase " + name, "Settings"));
        add(Descriptor("set." + name + ".down", "Decrease " + name, "Settings"));
        add(Descriptor("set." + name + ".value", "Set " + name, "Settings",
                       {Argument("value", "Value", VariantType::Integer)}));
    }
    add(Descriptor("set.skipread.value", "Set Skip Read", "Settings",
                   {Argument("value", "Value", VariantType::Bool)}));
    add(Descriptor("set.fullscreen.value", "Set Fullscreen", "Settings",
                   {Argument("value", "Value", VariantType::Bool)}));
    add(Descriptor("set.textscale.value", "Set Text Scale", "Accessibility",
                   {Argument("value", "Value", VariantType::Integer)}));
    add(Descriptor("set.highcontrast.value", "Set High Contrast", "Accessibility",
                   {Argument("value", "Value", VariantType::Bool)}));
    add(Descriptor("set.reducedmotion.value", "Set Reduced Motion", "Accessibility",
                   {Argument("value", "Value", VariantType::Bool)}));
    add(Descriptor("set.selfvoicing.value", "Set Self Voicing", "Accessibility",
                   {Argument("value", "Value", VariantType::Bool)}));
    return descriptors;
}

Status RegisterBuiltInActionDescriptors(ActionCatalog& catalog) {
    Status status;
    for (auto descriptor : BuiltInActionDescriptors()) {
        const Status registered = catalog.Register(std::move(descriptor));
        for (const auto& diagnostic : registered.Diagnostics()) status.Add(diagnostic);
    }
    return status;
}

}  // namespace px::ui
