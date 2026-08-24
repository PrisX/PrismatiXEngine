#pragma once

#include "Engine/Core/Variant.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::ui {

enum class ActionOrigin : std::uint8_t { BuiltIn, Plugin, ScriptExtension };
enum class ActionEditorHint : std::uint8_t {
    Default,
    Multiline,
    Enum,
    Color,
    Resource,
    Route,
    Node,
    Animation,
    Token,
};
enum class ActionReentryPolicy : std::uint8_t { Allow, IgnoreWhileRunning, Restart };

struct ActionArgumentDescriptor {
    std::string name;
    std::string displayName;
    std::string description;
    VariantType type = VariantType::Null;
    bool required = false;
    std::optional<Variant> defaultValue;
    ActionEditorHint editorHint = ActionEditorHint::Default;
    std::vector<std::string> enumValues;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::string resourceType;
};

struct ActionDescriptor {
    std::string id;
    std::string label;
    std::string category;
    std::string displayName;
    std::string description;
    ActionOrigin origin = ActionOrigin::BuiltIn;
    std::string sourceId;
    std::string providerId;
    std::vector<std::string> capabilities;
    std::vector<ActionArgumentDescriptor> arguments;
    ActionReentryPolicy reentryPolicy = ActionReentryPolicy::Allow;
    bool destructiveInPreview = false;
    bool available = true;
    std::string unavailableReason;
    bool allowAdditionalArguments = false;
};

struct ActionContext {
    std::string sourceScene;
    Uuid sourceNode;
    std::string signal;
    std::string currentRoute;
    void* runtimeServices = nullptr;
    bool preview = false;
};

struct ActionInvocation {
    std::string action;
    VariantObject arguments;
    ActionContext context;
};

[[nodiscard]] const char* ActionOriginName(ActionOrigin origin);
[[nodiscard]] const char* ActionReentryPolicyName(ActionReentryPolicy policy);
[[nodiscard]] std::optional<ActionReentryPolicy> ParseActionReentryPolicy(std::string_view value);

}  // namespace px::ui
