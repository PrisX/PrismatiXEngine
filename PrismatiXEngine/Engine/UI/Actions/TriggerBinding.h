#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/Actions/ActionCatalog.h"

#include <string>

namespace px::ui {

enum class TriggerBindingKind : std::uint8_t { Action, Flow };

struct TriggerBinding {
    Uuid node;
    std::string signal;
    TriggerBindingKind kind = TriggerBindingKind::Action;
    std::string action;
    VariantObject arguments;
    Uuid graphEntry;
    ActionReentryPolicy reentry = ActionReentryPolicy::Allow;
    std::string sourceScene;
    bool resolved = false;
    std::string resolutionMessage;

    [[nodiscard]] ActionInvocation Invocation() const;
    [[nodiscard]] Status Resolve(const ActionCatalog& catalog);
    [[nodiscard]] Variant ToVariant() const;
};

[[nodiscard]] const char* TriggerBindingKindName(TriggerBindingKind kind);

[[nodiscard]] Result<TriggerBinding> ParseTriggerBinding(
    const Uuid& node, std::string signal, const Variant& value,
    const std::string& sourceScene = {});

}  // namespace px::ui
