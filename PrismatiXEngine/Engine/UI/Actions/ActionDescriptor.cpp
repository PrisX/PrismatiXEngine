#include "Engine/UI/Actions/ActionDescriptor.h"

namespace px::ui {

const char* ActionOriginName(const ActionOrigin origin) {
    switch (origin) {
        case ActionOrigin::BuiltIn: return "BuiltIn";
        case ActionOrigin::Plugin: return "Plugin";
        case ActionOrigin::LuaExtension: return "LuaExtension";
    }
    return "BuiltIn";
}

const char* ActionReentryPolicyName(const ActionReentryPolicy policy) {
    switch (policy) {
        case ActionReentryPolicy::Allow: return "Allow";
        case ActionReentryPolicy::IgnoreWhileRunning: return "IgnoreWhileRunning";
        case ActionReentryPolicy::Restart: return "Restart";
    }
    return "Allow";
}

std::optional<ActionReentryPolicy> ParseActionReentryPolicy(const std::string_view value) {
    if (value == "Allow" || value == "allow") return ActionReentryPolicy::Allow;
    if (value == "IgnoreWhileRunning" || value == "ignoreWhileRunning" || value == "ignore")
        return ActionReentryPolicy::IgnoreWhileRunning;
    if (value == "Restart" || value == "restart") return ActionReentryPolicy::Restart;
    return std::nullopt;
}

}  // namespace px::ui
