#include "Engine/SDK/Ui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace px::sdk {
namespace {

using Json = nlohmann::json;

void AddDiagnostic(UiParseResult& result, std::string code,
                   std::string message, const std::size_t nodeIndex = 0) {
    result.diagnostics.push_back(
        {std::move(code), std::move(message), nodeIndex});
}

void AddDiagnostic(UiComponentParseResult& result, std::string code,
                   std::string message,
                   const std::size_t nodeIndex = 0) {
    result.diagnostics.push_back(
        {std::move(code), std::move(message), nodeIndex});
}

bool ReadString(const Json& object, const char* key, std::string& value,
                const bool allowEmpty = false) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() ||
        (!allowEmpty && found->empty())) return false;
    value = found->get<std::string>();
    return true;
}

bool IsUuid(const std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

bool IsColor(const std::string_view value) {
    if (value.size() != 7 && value.size() != 9) return false;
    if (value.front() != '#') return false;
    return std::all_of(value.begin() + 1, value.end(), [](const char value) {
        return std::isxdigit(static_cast<unsigned char>(value)) != 0;
    });
}

bool IsIdentifier(const std::string_view value) {
    if (value.empty()) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (value.front() != '_' && !std::isalpha(first)) return false;
    return std::all_of(value.begin() + 1, value.end(), [](const char value) {
        const auto byte = static_cast<unsigned char>(value);
        return value == '_' || std::isalnum(byte) != 0;
    });
}

bool IsPropertyPath(const std::string_view value) {
    if (value.empty() || value.size() > 256) return false;
    bool segmentStart = true;
    for (const char character : value) {
        if (character == '.') {
            if (segmentStart) return false;
            segmentStart = true;
            continue;
        }
        const auto byte = static_cast<unsigned char>(character);
        if (segmentStart) {
            if (character != '_' && !std::isalpha(byte)) return false;
            segmentStart = false;
        } else if (character != '_' && !std::isalnum(byte)) {
            return false;
        }
    }
    return !segmentStart;
}

std::optional<UiNodeKind> ParseKind(const std::string_view value) {
    if (value == "control") return UiNodeKind::Control;
    if (value == "label") return UiNodeKind::Label;
    if (value == "button") return UiNodeKind::Button;
    if (value == "image") return UiNodeKind::Image;
    if (value == "stack") return UiNodeKind::Stack;
    if (value == "hbox") return UiNodeKind::HBox;
    if (value == "vbox") return UiNodeKind::VBox;
    if (value == "grid") return UiNodeKind::Grid;
    if (value == "group") return UiNodeKind::Group;
    if (value == "leaf") return UiNodeKind::Leaf;
    return std::nullopt;
}

bool OwnsLayout(const UiNodeKind kind) {
    return kind == UiNodeKind::Stack || kind == UiNodeKind::HBox ||
           kind == UiNodeKind::VBox || kind == UiNodeKind::Grid;
}

bool ReadFiniteFloat(const Json& object, const char* key, float& value) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return false;
    value = found->get<float>();
    return std::isfinite(value);
}

bool IsJsonValue(const Json& value);

bool ParseValue(const Json& value, UiValue& output) {
    if (value.is_null()) output = std::monostate{};
    else if (value.is_boolean()) output = value.get<bool>();
    else if (value.is_number_integer()) output = value.get<std::int64_t>();
    else if (value.is_number()) output = value.get<double>();
    else if (value.is_string()) output = value.get<std::string>();
    else if (value.is_array()) {
        if (!IsJsonValue(value)) return false;
        output = UiArrayValue{value.dump()};
    } else if (value.is_object()) {
        const auto typeValue = value.find("type");
        const std::string type =
            typeValue != value.end() && typeValue->is_string()
                ? typeValue->get<std::string>()
                : std::string{};
        if (type == "vec2") {
            UiVec2Value parsed;
            if (value.size() != 3 || !ReadFiniteFloat(value, "x", parsed.x) ||
                !ReadFiniteFloat(value, "y", parsed.y)) return false;
            output = parsed;
        } else if (type == "rect") {
            UiRectValue parsed;
            if (value.size() != 5 || !ReadFiniteFloat(value, "x", parsed.x) ||
                !ReadFiniteFloat(value, "y", parsed.y) ||
                !ReadFiniteFloat(value, "width", parsed.width) ||
                !ReadFiniteFloat(value, "height", parsed.height)) return false;
            output = parsed;
        } else if (type == "color") {
            UiColorValue parsed;
            if (value.size() != 2 || !ReadString(value, "value", parsed.value) ||
                !IsColor(parsed.value)) return false;
            output = std::move(parsed);
        } else if (type == "uuid") {
            UiUuidValue parsed;
            if (value.size() != 2 || !ReadString(value, "value", parsed.value) ||
                !IsUuid(parsed.value)) return false;
            output = std::move(parsed);
        } else if (type == "resource") {
            UiResourceValue parsed;
            if (value.size() != 2 || !ReadString(value, "value", parsed.value) ||
                (!parsed.value.empty() && !IsUuid(parsed.value))) return false;
            output = std::move(parsed);
        } else if (type == "token") {
            UiTokenValue parsed;
            if (value.size() != 2 || !ReadString(value, "value", parsed.value) ||
                parsed.value.empty()) return false;
            output = std::move(parsed);
        } else if (type == "nodeReference") {
            UiNodeReferenceValue parsed;
            if (value.size() != 2 || !ReadString(value, "nodeId", parsed.nodeId) ||
                !IsUuid(parsed.nodeId)) return false;
            output = std::move(parsed);
        } else {
            if (!IsJsonValue(value)) return false;
            output = UiObjectValue{value.dump()};
        }
    } else return false;
    return true;
}

bool ParseActionValue(const Json& value, UiActionValue& output) {
    if (!ParseValue(value, output)) return false;
    return output.index() <= 4;
}

bool KnownAction(std::string_view id);

bool IsJsonValueImpl(const Json& value, const std::size_t depth,
                     std::size_t& nodes) {
    constexpr std::size_t kMaxDepth = 32;
    constexpr std::size_t kMaxNodes = 8192;
    constexpr std::size_t kMaxArrayEntries = 1024;
    constexpr std::size_t kMaxObjectEntries = 256;
    if (depth > kMaxDepth || ++nodes > kMaxNodes) return false;
    if (value.is_null() || value.is_boolean() || value.is_string() ||
        value.is_number_integer())
        return true;
    if (value.is_number_float()) return std::isfinite(value.get<double>());
    if (value.is_array()) {
        if (value.size() > kMaxArrayEntries) return false;
        return std::ranges::all_of(value, [&](const Json& item) {
            return IsJsonValueImpl(item, depth + 1, nodes);
        });
    }
    if (value.is_object()) {
        if (value.size() > kMaxObjectEntries) return false;
        return std::ranges::all_of(value.items(), [&](const auto& item) {
            return !item.key().empty() && item.key().size() <= 256 &&
                   IsJsonValueImpl(item.value(), depth + 1, nodes);
        });
    }
    return false;
}

bool IsJsonValue(const Json& value) {
    constexpr std::size_t kMaxJsonBytes = 1024 * 1024;
    if (value.dump().size() > kMaxJsonBytes) return false;
    std::size_t nodes = 0;
    return IsJsonValueImpl(value, 0, nodes);
}

bool ParsePublicSignalBinding(
    const Json& value, const std::uint32_t schemaRevision,
    std::optional<UiComponentSignalBinding>& output) {
    if (value.is_null()) {
        output.reset();
        return true;
    }
    if (!value.is_object() || value.size() < 2 || value.size() > 3 ||
        (schemaRevision == 1 && value.size() != 2))
        return false;
    for (auto item = value.begin(); item != value.end(); ++item)
        if (item.key() != "id" && item.key() != "arguments" &&
            item.key() != "argumentBindings")
            return false;
    UiComponentSignalBinding binding;
    if (!ReadString(value, "id", binding.action.id) ||
        !KnownAction(binding.action.id))
        return false;
    const auto arguments = value.find("arguments");
    if (arguments == value.end() || !arguments->is_object()) return false;
    for (auto argument = arguments->begin(); argument != arguments->end();
         ++argument) {
        UiActionValue parsed;
        if (argument.key().empty() ||
            !ParseActionValue(argument.value(), parsed))
            return false;
        binding.action.arguments.emplace(argument.key(), std::move(parsed));
    }
    const auto mappings = value.find("argumentBindings");
    if (mappings != value.end()) {
        const auto identity = [](const std::string_view name) {
            if (name.empty() || name.size() > 128 ||
                !(std::isalpha(static_cast<unsigned char>(name.front())) ||
                  name.front() == '_'))
                return false;
            return std::ranges::all_of(name.substr(1), [](const char character) {
                return std::isalnum(static_cast<unsigned char>(character)) ||
                       character == '_' || character == '.' || character == '-';
            });
        };
        if (schemaRevision != 2 || !mappings->is_object() ||
            mappings->size() > 256)
            return false;
        for (auto mapping = mappings->begin(); mapping != mappings->end();
             ++mapping) {
            if (!identity(mapping.key()) || !mapping.value().is_string())
                return false;
            auto source = mapping.value().get<std::string>();
            if (!identity(source)) return false;
            binding.argumentBindings.emplace(mapping.key(), std::move(source));
        }
    }
    output = std::move(binding);
    return true;
}

bool ReadValueMap(const Json& value,
                  std::unordered_map<std::string, UiValue>& output) {
    if (!value.is_object()) return false;
    for (auto item = value.begin(); item != value.end(); ++item) {
        UiValue parsed;
        if (item.key().empty() || !ParseValue(item.value(), parsed)) return false;
        output.emplace(item.key(), std::move(parsed));
    }
    return true;
}

std::optional<UiBehaviorNodeKind> ParseBehaviorKind(
    const std::string_view value) {
    if (value == "signalEntry") return UiBehaviorNodeKind::SignalEntry;
    if (value == "action") return UiBehaviorNodeKind::Action;
    if (value == "sequence") return UiBehaviorNodeKind::Sequence;
    if (value == "branch") return UiBehaviorNodeKind::Branch;
    if (value == "delay") return UiBehaviorNodeKind::Delay;
    if (value == "constant") return UiBehaviorNodeKind::Constant;
    if (value == "compare") return UiBehaviorNodeKind::Compare;
    if (value == "boolean") return UiBehaviorNodeKind::Boolean;
    if (value == "getVariable") return UiBehaviorNodeKind::GetVariable;
    if (value == "setVariable") return UiBehaviorNodeKind::SetVariable;
    if (value == "getProperty") return UiBehaviorNodeKind::GetProperty;
    if (value == "setProperty") return UiBehaviorNodeKind::SetProperty;
    if (value == "playAnimation") return UiBehaviorNodeKind::PlayAnimation;
    if (value == "setAnimationParameter")
        return UiBehaviorNodeKind::SetAnimationParameter;
    if (value == "travelAnimationState")
        return UiBehaviorNodeKind::TravelAnimationState;
    return std::nullopt;
}

std::optional<UiBehaviorReentry> ParseBehaviorReentry(
    const std::string_view value) {
    if (value == "allow") return UiBehaviorReentry::Allow;
    if (value == "ignoreWhileRunning")
        return UiBehaviorReentry::IgnoreWhileRunning;
    if (value == "restart") return UiBehaviorReentry::Restart;
    return std::nullopt;
}

std::optional<UiAnimationEase> ParseAnimationEase(
    const std::string_view value) {
    if (value == "linear") return UiAnimationEase::Linear;
    if (value == "easeIn") return UiAnimationEase::EaseIn;
    if (value == "easeOut") return UiAnimationEase::EaseOut;
    if (value == "easeInOut") return UiAnimationEase::EaseInOut;
    if (value == "step") return UiAnimationEase::Step;
    return std::nullopt;
}

std::optional<UiVisualStateEase> ParseVisualStateEase(
    const std::string_view value) {
    if (value == "step") return UiVisualStateEase::Step;
    if (value == "linear") return UiVisualStateEase::Linear;
    if (value == "easeIn") return UiVisualStateEase::EaseIn;
    if (value == "easeOut") return UiVisualStateEase::EaseOut;
    if (value == "easeInOut") return UiVisualStateEase::EaseInOut;
    if (value == "backOut") return UiVisualStateEase::BackOut;
    return std::nullopt;
}

std::optional<UiAnimationInterpolation> ParseAnimationInterpolation(
    const std::string_view value) {
    if (value == "linear") return UiAnimationInterpolation::Linear;
    if (value == "discrete") return UiAnimationInterpolation::Discrete;
    return std::nullopt;
}

std::optional<UiAnimationParameterType> ParseAnimationParameterType(
    const std::string_view value) {
    if (value == "trigger") return UiAnimationParameterType::Trigger;
    if (value == "bool") return UiAnimationParameterType::Bool;
    if (value == "number") return UiAnimationParameterType::Number;
    return std::nullopt;
}

std::optional<UiAnimationConditionOperator> ParseAnimationCondition(
    const std::string_view value) {
    if (value == "triggered")
        return UiAnimationConditionOperator::Triggered;
    if (value == "equal") return UiAnimationConditionOperator::Equal;
    if (value == "notEqual") return UiAnimationConditionOperator::NotEqual;
    if (value == "less") return UiAnimationConditionOperator::Less;
    if (value == "lessEqual")
        return UiAnimationConditionOperator::LessEqual;
    if (value == "greater") return UiAnimationConditionOperator::Greater;
    if (value == "greaterEqual")
        return UiAnimationConditionOperator::GreaterEqual;
    return std::nullopt;
}

const UiValue* Property(const UiBehaviorNode& node,
                              const std::string_view name) {
    const auto found = node.properties.find(std::string(name));
    return found == node.properties.end() ? nullptr : &found->second;
}

bool IsNumber(const UiValue& value) {
    return std::holds_alternative<std::int64_t>(value) ||
           std::holds_alternative<double>(value);
}

std::optional<double> Number(const UiValue* value) {
    if (!value) return std::nullopt;
    if (const auto* integer = std::get_if<std::int64_t>(value))
        return static_cast<double>(*integer);
    if (const auto* number = std::get_if<double>(value)) return *number;
    return std::nullopt;
}

const std::string* String(const UiValue* value) {
    return value ? std::get_if<std::string>(value) : nullptr;
}

bool KnownAction(const std::string_view id) {
    static const std::unordered_set<std::string_view> actions = {
        "game.start", "app.quit", "overlay.close", "load.open", "save.open",
        "gallery.open", "settings.open", "backlog.open", "choice.select",
        "load.slot", "save.slot", "cg.view", "mode.auto", "mode.skip",
        "set.skipread.toggle", "set.fullscreen.toggle", "animation.trigger",
        "animation.bool", "animation.number", "animation.travel"};
    if (actions.contains(id)) return true;
    if (id.empty() || id.front() == '.' || id.back() == '.' ||
        id.find('.') == std::string_view::npos)
        return false;
    bool segmentHasCharacter = false;
    for (const char character : id) {
        if (character == '.') {
            if (!segmentHasCharacter) return false;
            segmentHasCharacter = false;
            continue;
        }
        if (!(std::isalnum(static_cast<unsigned char>(character)) ||
              character == '_' || character == '-'))
            return false;
        segmentHasCharacter = true;
    }
    return segmentHasCharacter;
}

bool ValidAction(const UiAction& action) {
    static const std::unordered_set<std::string> noArguments = {
        "game.start", "app.quit", "overlay.close", "load.open", "save.open",
        "gallery.open", "settings.open", "backlog.open", "mode.auto", "mode.skip",
        "set.skipread.toggle", "set.fullscreen.toggle"};
    if (noArguments.contains(action.id)) return action.arguments.empty();
    if (action.id == "choice.select" || action.id == "load.slot" ||
        action.id == "save.slot") {
        const char* name = action.id == "choice.select" ? "index" : "slot";
        const auto found = action.arguments.find(name);
        return action.arguments.size() == 1 && found != action.arguments.end() &&
               std::holds_alternative<std::int64_t>(found->second);
    }
    if (action.id == "cg.view") {
        const auto found = action.arguments.find("resource");
        return action.arguments.size() == 1 && found != action.arguments.end() &&
               std::holds_alternative<std::string>(found->second);
    }
    // Extension Action identity and parameter contracts are owned by the
    // versioned runtime ActionCatalog loaded from the extension manifest. The
    // UI document parser only enforces a safe namespaced identity and values it
    // can round-trip; dispatch performs catalog/provider validation.
    return KnownAction(action.id);
}

bool SupportedRuntimeProperty(const std::string_view property) {
    static const std::unordered_set<std::string_view> names = {
        "opacity", "rotation", "scale", "pivot", "modulate", "visibility",
        "enabled", "offsets", "minimumSize"};
    return names.contains(property);
}

bool RuntimePropertyValueMatches(const std::string_view property,
                                 const UiValue& value) {
    if (property == "opacity" || property == "rotation") return IsNumber(value);
    if (property == "scale" || property == "pivot" ||
        property == "minimumSize")
        return std::holds_alternative<UiVec2Value>(value);
    if (property == "modulate")
        return std::holds_alternative<UiColorValue>(value);
    if (property == "visibility")
        return std::get_if<std::string>(&value) &&
               (*std::get_if<std::string>(&value) == "Visible" ||
                *std::get_if<std::string>(&value) == "Hidden" ||
                *std::get_if<std::string>(&value) == "Collapsed");
    if (property == "enabled") return std::holds_alternative<bool>(value);
    if (property == "offsets")
        return std::holds_alternative<UiRectValue>(value);
    return false;
}

bool ValidSignal(const UiNodeKind kind, const std::string_view signal) {
    static const std::unordered_set<std::string_view> controlSignals = {
        "pointerEntered", "pointerExited", "pointerDown", "pointerUp", "clicked",
        "scrolled", "focusEntered", "focusExited"};
    return controlSignals.contains(signal) ||
           (kind == UiNodeKind::Button && signal == "activated");
}

bool AllowedProperties(
    const UiBehaviorNode& node,
    const std::initializer_list<std::string_view> allowed) {
    return std::all_of(node.properties.begin(), node.properties.end(),
                       [&](const auto& item) {
                           return std::find(allowed.begin(), allowed.end(),
                                            item.first) != allowed.end();
                       });
}

bool ValidateBehaviorNode(const UiBehaviorNode& node,
                          const std::unordered_set<std::string>& uiNodeIds) {
    using Kind = UiBehaviorNodeKind;
    if (!std::isfinite(node.x) || !std::isfinite(node.y)) return false;
    if (node.kind != Kind::Action && !node.arguments.empty()) return false;
    if (node.kind == Kind::SignalEntry || node.kind == Kind::Sequence)
        return node.properties.empty();
    if (node.kind == Kind::Action) {
        const auto* action = String(Property(node, "action"));
        const auto* wait = Property(node, "wait");
        return AllowedProperties(node, {"action", "wait"}) && action &&
               KnownAction(*action) &&
               (!wait || std::holds_alternative<bool>(*wait));
    }
    if (node.kind == Kind::Branch) {
        const auto* condition = Property(node, "condition");
        return AllowedProperties(node, {"condition"}) &&
               (!condition || std::holds_alternative<bool>(*condition));
    }
    if (node.kind == Kind::Delay) {
        const auto seconds = Number(Property(node, "seconds"));
        return AllowedProperties(node, {"seconds"}) && seconds && *seconds >= 0.0;
    }
    if (node.kind == Kind::Constant)
        return AllowedProperties(node, {"value"}) && Property(node, "value");
    if (node.kind == Kind::Compare) {
        const auto* operation = String(Property(node, "operator"));
        static const std::unordered_set<std::string_view> operations = {
            "Equal", "NotEqual", "Less", "LessEqual", "Greater",
            "GreaterEqual"};
        return AllowedProperties(node, {"left", "right", "operator"}) &&
               operation && operations.contains(*operation);
    }
    if (node.kind == Kind::Boolean) {
        const auto* operation = String(Property(node, "operator"));
        const auto* left = Property(node, "left");
        const auto* right = Property(node, "right");
        return AllowedProperties(node, {"left", "right", "operator"}) &&
               operation &&
               (*operation == "Not" || *operation == "And" ||
                *operation == "Or") &&
               (!left || std::holds_alternative<bool>(*left)) &&
               (!right || std::holds_alternative<bool>(*right));
    }
    if (node.kind == Kind::GetVariable || node.kind == Kind::SetVariable) {
        const auto* name = String(Property(node, "name"));
        return AllowedProperties(
                   node, node.kind == Kind::GetVariable
                             ? std::initializer_list<std::string_view>{"name"}
                             : std::initializer_list<std::string_view>{"name",
                                                                       "value"}) &&
               name && IsIdentifier(*name) &&
               (node.kind == Kind::GetVariable || Property(node, "value"));
    }
    if (node.kind == Kind::GetProperty || node.kind == Kind::SetProperty) {
        const auto* target =
            Property(node, "target")
                ? std::get_if<UiNodeReferenceValue>(
                      Property(node, "target"))
                : nullptr;
        const auto* property = String(Property(node, "property"));
        const auto* value = Property(node, "value");
        return AllowedProperties(
                   node, node.kind == Kind::GetProperty
                             ? std::initializer_list<std::string_view>{
                                   "target", "property"}
                             : std::initializer_list<std::string_view>{
                                   "target", "property", "value"}) &&
               target && uiNodeIds.contains(target->nodeId) && property &&
               SupportedRuntimeProperty(*property) &&
               (node.kind == Kind::GetProperty ||
                (value && RuntimePropertyValueMatches(*property, *value)));
    }
    if (node.kind == Kind::PlayAnimation) {
        const auto* name = String(Property(node, "name"));
        const auto* wait = Property(node, "wait");
        return AllowedProperties(node, {"name", "wait"}) && name &&
               !name->empty() && (!wait || std::holds_alternative<bool>(*wait));
    }
    if (node.kind == Kind::SetAnimationParameter) {
        const auto* name = String(Property(node, "name"));
        return AllowedProperties(node, {"name", "value"}) && name &&
               IsIdentifier(*name) && Property(node, "value");
    }
    const auto* state = String(Property(node, "state"));
    const auto duration = Number(Property(node, "duration"));
    return AllowedProperties(node, {"state", "duration"}) && state &&
           !state->empty() && (!duration || *duration >= 0.0);
}

bool SameAnimationValueType(const UiValue& left,
                            const UiValue& right) {
    if (IsNumber(left) && IsNumber(right)) return true;
    return left.index() == right.index();
}

bool BehaviorOutputPin(const UiBehaviorNode& node,
                       const std::string_view pin) {
    using Kind = UiBehaviorNodeKind;
    if (node.kind == Kind::SignalEntry)
        return pin == "out" || pin.starts_with("arg:");
    if (node.kind == Kind::Action || node.kind == Kind::Delay ||
        node.kind == Kind::SetVariable || node.kind == Kind::SetProperty ||
        node.kind == Kind::PlayAnimation ||
        node.kind == Kind::SetAnimationParameter ||
        node.kind == Kind::TravelAnimationState)
        return pin == "out";
    if (node.kind == Kind::Sequence) return !pin.empty();
    if (node.kind == Kind::Branch) return pin == "true" || pin == "false";
    if (node.kind == Kind::Constant || node.kind == Kind::Compare ||
        node.kind == Kind::Boolean || node.kind == Kind::GetVariable ||
        node.kind == Kind::GetProperty)
        return pin == "value";
    return false;
}

bool BehaviorInputPin(const UiBehaviorNode& node,
                      const std::string_view pin) {
    using Kind = UiBehaviorNodeKind;
    if (node.kind == Kind::Action)
        return pin == "in" || pin.starts_with("arg:");
    if (node.kind == Kind::Sequence) return pin == "in";
    if (node.kind == Kind::SetVariable || node.kind == Kind::SetProperty ||
        node.kind == Kind::SetAnimationParameter)
        return pin == "in" || pin == "value";
    if (node.kind == Kind::Branch)
        return pin == "in" || pin == "condition";
    if (node.kind == Kind::Delay)
        return pin == "in" || pin == "seconds";
    if (node.kind == Kind::Compare)
        return pin == "left" || pin == "right";
    if (node.kind == Kind::Boolean)
        return pin == "left" || pin == "right";
    if (node.kind == Kind::PlayAnimation ||
        node.kind == Kind::TravelAnimationState)
        return pin == "in";
    return false;
}

bool FlowOutputPin(const UiBehaviorNode& node,
                   const std::string_view pin) {
    using Kind = UiBehaviorNodeKind;
    if (node.kind == Kind::SignalEntry || node.kind == Kind::Action ||
        node.kind == Kind::Delay || node.kind == Kind::SetVariable ||
        node.kind == Kind::SetProperty ||
        node.kind == Kind::PlayAnimation ||
        node.kind == Kind::SetAnimationParameter ||
        node.kind == Kind::TravelAnimationState)
        return pin == "out";
    if (node.kind == Kind::Sequence) return !pin.empty();
    if (node.kind == Kind::Branch) return pin == "true" || pin == "false";
    return false;
}

void ParseBehaviorSections(
    const Json& root, UiParseResult& result,
    std::unordered_set<std::string>& identities,
    const std::unordered_map<std::string, const UiNode*>& uiNodes) {
    const auto graphValue = root.find("behaviorGraph");
    if (graphValue != root.end()) {
        if (!graphValue->is_object()) {
            AddDiagnostic(result, "PXSDKUI1050",
                          "UI document behaviorGraph must be an object");
        } else {
            const auto nodes = graphValue->find("nodes");
            const auto links = graphValue->find("links");
            const auto groups = graphValue->find("groups");
            if (nodes == graphValue->end() || !nodes->is_array() ||
                links == graphValue->end() || !links->is_array() ||
                groups == graphValue->end() || !groups->is_array()) {
                AddDiagnostic(
                    result, "PXSDKUI1051",
                    "Behavior Graph requires nodes, links, and groups arrays");
            } else {
                for (const auto& item : *nodes) {
                    UiBehaviorNode node;
                    std::string kind;
                    const auto position = item.is_object()
                                              ? item.find("position")
                                              : item.end();
                    const auto properties = item.is_object()
                                                ? item.find("properties")
                                                : item.end();
                    const auto arguments = item.is_object()
                                               ? item.find("arguments")
                                               : item.end();
                    const auto parsedKind =
                        item.is_object() && ReadString(item, "kind", kind)
                            ? ParseBehaviorKind(kind)
                            : std::nullopt;
                    if (!kind.empty() && !parsedKind) {
                        AddDiagnostic(
                            result, "PXSDKUI1059",
                            "Unsupported Behavior node kind cannot execute: " +
                                kind);
                        continue;
                    }
                    if (!item.is_object() ||
                        !ReadString(item, "id", node.id) ||
                        !IsUuid(node.id) ||
                        !identities.insert(node.id).second || !parsedKind ||
                        position == item.end() || !position->is_object() ||
                        !ReadFiniteFloat(*position, "x", node.x) ||
                        !ReadFiniteFloat(*position, "y", node.y) ||
                        properties == item.end() ||
                        !ReadValueMap(*properties, node.properties) ||
                        arguments == item.end() ||
                        !ReadValueMap(*arguments, node.arguments)) {
                        AddDiagnostic(
                            result, "PXSDKUI1052",
                            "Behavior node requires a unique UUID, named kind, "
                            "finite position, properties, and arguments");
                        continue;
                    }
                    node.kind = *parsedKind;
                    result.document.behaviorGraph.nodes.push_back(
                        std::move(node));
                }
                for (const auto& item : *links) {
                    UiBehaviorLink link;
                    if (!item.is_object() ||
                        !ReadString(item, "id", link.id) ||
                        !IsUuid(link.id) ||
                        !identities.insert(link.id).second ||
                        !ReadString(item, "fromNodeId", link.fromNodeId) ||
                        !IsUuid(link.fromNodeId) ||
                        !ReadString(item, "fromPin", link.fromPin) ||
                        !ReadString(item, "toNodeId", link.toNodeId) ||
                        !IsUuid(link.toNodeId) ||
                        !ReadString(item, "toPin", link.toPin)) {
                        AddDiagnostic(result, "PXSDKUI1053",
                                      "Behavior link fields are invalid");
                        continue;
                    }
                    result.document.behaviorGraph.links.push_back(
                        std::move(link));
                }
                for (const auto& item : *groups) {
                    UiBehaviorGroup group;
                    const auto bounds =
                        item.is_object() ? item.find("bounds") : item.end();
                    if (!item.is_object() ||
                        !ReadString(item, "id", group.id) ||
                        !IsUuid(group.id) ||
                        !identities.insert(group.id).second ||
                        !ReadString(item, "title", group.title) ||
                        bounds == item.end() || !bounds->is_object() ||
                        !ReadFiniteFloat(*bounds, "x", group.bounds.x) ||
                        !ReadFiniteFloat(*bounds, "y", group.bounds.y) ||
                        !ReadFiniteFloat(*bounds, "width",
                                         group.bounds.width) ||
                        !ReadFiniteFloat(*bounds, "height",
                                         group.bounds.height) ||
                        group.bounds.width <= 0.0f ||
                        group.bounds.height <= 0.0f) {
                        AddDiagnostic(result, "PXSDKUI1054",
                                      "Behavior group fields are invalid");
                        continue;
                    }
                    result.document.behaviorGraph.groups.push_back(
                        std::move(group));
                }
            }
        }
    }

    std::unordered_map<std::string, const UiBehaviorNode*> graphNodes;
    std::unordered_set<std::string> uiNodeIds;
    for (const auto& [id, _] : uiNodes) uiNodeIds.insert(id);
    for (const auto& node : result.document.behaviorGraph.nodes) {
        graphNodes.emplace(node.id, &node);
        if (!ValidateBehaviorNode(node, uiNodeIds))
            AddDiagnostic(result, "PXSDKUI1055",
                          "Behavior node properties do not match its named kind");
    }
    std::unordered_set<std::string> occupiedInputs;
    std::unordered_map<std::string, std::vector<std::string>> flow;
    for (const auto& link : result.document.behaviorGraph.links) {
        const auto from = graphNodes.find(link.fromNodeId);
        const auto to = graphNodes.find(link.toNodeId);
        const std::string input = link.toNodeId + "/" + link.toPin;
        if (from == graphNodes.end() || to == graphNodes.end() ||
            link.fromNodeId == link.toNodeId ||
            !BehaviorOutputPin(*from->second, link.fromPin) ||
            !BehaviorInputPin(*to->second, link.toPin) ||
            !occupiedInputs.insert(input).second) {
            AddDiagnostic(result, "PXSDKUI1056",
                          "Behavior link endpoint or pin is invalid");
            continue;
        }
        if (FlowOutputPin(*from->second, link.fromPin)) {
            if (link.toPin != "in")
                AddDiagnostic(result, "PXSDKUI1057",
                              "Behavior flow output must connect to a flow input");
            else flow[link.fromNodeId].push_back(link.toNodeId);
        }
    }
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    const auto hasCycle = [&](const auto& self,
                              const std::string& id) -> bool {
        if (visiting.contains(id)) return true;
        if (visited.contains(id)) return false;
        visiting.insert(id);
        for (const auto& next : flow[id])
            if (self(self, next)) return true;
        visiting.erase(id);
        visited.insert(id);
        return false;
    };
    for (const auto& [id, _] : graphNodes)
        if (hasCycle(hasCycle, id)) {
            AddDiagnostic(result, "PXSDKUI1058",
                          "Behavior Graph flow cannot contain a cycle");
            break;
        }

    const auto triggers = root.find("behaviorTriggers");
    if (triggers == root.end()) return;
    if (!triggers->is_array()) {
        AddDiagnostic(result, "PXSDKUI1060",
                      "UI document behaviorTriggers must be an array");
        return;
    }
    std::unordered_set<std::string> triggerSources;
    for (const auto& item : *triggers) {
        UiBehaviorTrigger trigger;
        std::string reentry;
        const auto parsedReentry =
            item.is_object() && ReadString(item, "reentry", reentry)
                ? ParseBehaviorReentry(reentry)
                : std::nullopt;
        if (!item.is_object() || !ReadString(item, "id", trigger.id) ||
            !IsUuid(trigger.id) || !identities.insert(trigger.id).second ||
            !ReadString(item, "nodeId", trigger.nodeId) ||
            !IsUuid(trigger.nodeId) ||
            !ReadString(item, "signal", trigger.signal) ||
            !ReadString(item, "entryNodeId", trigger.entryNodeId) ||
            !IsUuid(trigger.entryNodeId) || !parsedReentry) {
            AddDiagnostic(result, "PXSDKUI1061",
                          "Behavior trigger fields are invalid");
            continue;
        }
        trigger.reentry = *parsedReentry;
        const auto source = uiNodes.find(trigger.nodeId);
        const auto entry = graphNodes.find(trigger.entryNodeId);
        const std::string sourceSignal = trigger.nodeId + "/" + trigger.signal;
        const bool dynamicSignal =
            result.document.schemaRevision == 2 && source != uiNodes.end() &&
            source->second->runtimeType.has_value() &&
            !source->second->runtimeType->empty() && !trigger.signal.empty() &&
            trigger.signal.size() <= 128;
        if (source == uiNodes.end() ||
            !(ValidSignal(source->second->kind, trigger.signal) ||
              dynamicSignal) ||
            entry == graphNodes.end() ||
            entry->second->kind != UiBehaviorNodeKind::SignalEntry ||
            !triggerSources.insert(sourceSignal).second) {
            AddDiagnostic(
                result, "PXSDKUI1062",
                "Behavior trigger must reference one valid signal and Signal Entry");
            continue;
        }
        result.document.behaviorTriggers.push_back(std::move(trigger));
    }
}

void ParseVisualStateSection(
    const Json& root, UiParseResult& result,
    const std::unordered_map<std::string, const UiNode*>& uiNodes) {
    const auto groups = root.find("visualStateGroups");
    if (groups == root.end()) return;
    if (!groups->is_array() || groups->size() > 10'000) {
        AddDiagnostic(result, "PXSDKUI1066",
                      "UI document visualStateGroups must be a bounded array");
        return;
    }
    std::unordered_set<std::string> groupIds;
    for (const auto& item : *groups) {
        UiVisualStateGroup group;
        const auto states = item.is_object() ? item.find("states") : item.end();
        const auto transitions =
            item.is_object() ? item.find("transitions") : item.end();
        if (!item.is_object() || !ReadString(item, "id", group.id) ||
            !IsIdentifier(group.id) || !groupIds.insert(group.id).second ||
            !ReadString(item, "defaultState", group.defaultState) ||
            !IsIdentifier(group.defaultState) || states == item.end() ||
            !states->is_array() || states->empty() ||
            transitions == item.end() || !transitions->is_array()) {
            AddDiagnostic(result, "PXSDKUI1067",
                          "Visual State Group fields are invalid");
            continue;
        }
        std::unordered_set<std::string> stateIds;
        for (const auto& stateValue : *states) {
            UiVisualState state;
            const auto overrides = stateValue.is_object()
                                       ? stateValue.find("overrides")
                                       : stateValue.end();
            if (!stateValue.is_object() ||
                !ReadString(stateValue, "id", state.id) ||
                !IsIdentifier(state.id) ||
                !stateIds.insert(state.id).second ||
                overrides == stateValue.end() || !overrides->is_array()) {
                AddDiagnostic(result, "PXSDKUI1068",
                              "Visual State fields are invalid");
                continue;
            }
            std::unordered_set<std::string> targets;
            for (const auto& overrideValue : *overrides) {
                UiVisualStateOverride stateOverride;
                const auto authoredValue = overrideValue.is_object()
                                               ? overrideValue.find("value")
                                               : overrideValue.end();
                if (!overrideValue.is_object() ||
                    !ReadString(overrideValue, "nodeId",
                                stateOverride.nodeId) ||
                    !IsUuid(stateOverride.nodeId) ||
                    !uiNodes.contains(stateOverride.nodeId) ||
                    !ReadString(overrideValue, "property",
                                stateOverride.property) ||
                    !IsPropertyPath(stateOverride.property) ||
                    authoredValue == overrideValue.end() ||
                    !ParseValue(*authoredValue, stateOverride.value) ||
                    !targets.insert(stateOverride.nodeId + "/" +
                                    stateOverride.property)
                         .second) {
                    AddDiagnostic(result, "PXSDKUI1069",
                                  "Visual State override is invalid");
                    continue;
                }
                state.overrides.push_back(std::move(stateOverride));
            }
            group.states.push_back(std::move(state));
        }
        if (!stateIds.contains(group.defaultState)) {
            AddDiagnostic(result, "PXSDKUI1067",
                          "Visual State Group defaultState is unresolved");
            continue;
        }
        for (const auto& transitionValue : *transitions) {
            UiVisualStateTransition transition;
            std::string easing;
            const auto duration = transitionValue.is_object()
                                      ? transitionValue.find("duration")
                                      : transitionValue.end();
            const auto animationClipId = transitionValue.is_object()
                                             ? transitionValue.find(
                                                   "animationClipId")
                                             : transitionValue.end();
            const auto parsedEase =
                transitionValue.is_object() &&
                        ReadString(transitionValue, "easing", easing)
                    ? ParseVisualStateEase(easing)
                    : std::nullopt;
            if (!transitionValue.is_object() ||
                !ReadString(transitionValue, "from", transition.from) ||
                !stateIds.contains(transition.from) ||
                !ReadString(transitionValue, "to", transition.to) ||
                !stateIds.contains(transition.to) ||
                duration == transitionValue.end() || !duration->is_number() ||
                !std::isfinite(transition.duration = duration->get<float>()) ||
                transition.duration < 0.0f || !parsedEase ||
                (animationClipId != transitionValue.end() &&
                 (!animationClipId->is_string() ||
                  !IsUuid(animationClipId->get_ref<const std::string&>())))) {
                AddDiagnostic(result, "PXSDKUI1069",
                              "Visual State transition is invalid");
                continue;
            }
            transition.easing = *parsedEase;
            if (animationClipId != transitionValue.end())
                transition.animationClipId =
                    animationClipId->get<std::string>();
            group.transitions.push_back(std::move(transition));
        }
        result.document.visualStateGroups.push_back(std::move(group));
    }
}

void ParseAnimationSection(
    const Json& root, UiParseResult& result,
    std::unordered_set<std::string>& identities,
    const std::unordered_map<std::string, const UiNode*>& uiNodes) {
    const auto value = root.find("animations");
    if (value == root.end() || value->is_null()) return;
    if (!value->is_object()) {
        AddDiagnostic(result, "PXSDKUI1070",
                      "UI document animations must be an object or null");
        return;
    }
    UiAnimations animations;
    const auto clips = value->find("clips");
    const auto machine = value->find("stateMachine");
    if (clips == value->end() || !clips->is_array() ||
        machine == value->end() || !machine->is_object()) {
        AddDiagnostic(
            result, "PXSDKUI1071",
            "Animations require clips and a named stateMachine object");
        return;
    }

    std::unordered_map<std::string, const UiNode*> nodes = uiNodes;
    std::unordered_set<std::string> clipIds;
    std::unordered_set<std::string> clipNames;
    for (const auto& item : *clips) {
        UiAnimationClip clip;
        const auto duration = item.is_object() ? item.find("duration") : item.end();
        const auto loop = item.is_object() ? item.find("loop") : item.end();
        const auto tracks = item.is_object() ? item.find("tracks") : item.end();
        if (!item.is_object() || !ReadString(item, "id", clip.id) ||
            !IsUuid(clip.id) || !identities.insert(clip.id).second ||
            !clipIds.insert(clip.id).second ||
            !ReadString(item, "name", clip.name) ||
            !clipNames.insert(clip.name).second || duration == item.end() ||
            !duration->is_number() ||
            !std::isfinite(clip.duration = duration->get<float>()) ||
            clip.duration < 0.0f || loop == item.end() ||
            !loop->is_boolean() || tracks == item.end() ||
            !tracks->is_array()) {
            AddDiagnostic(result, "PXSDKUI1072",
                          "Animation clip fields are invalid");
            continue;
        }
        clip.loop = loop->get<bool>();
        for (const auto& trackValue : *tracks) {
            UiAnimationTrack track;
            const auto keys = trackValue.is_object()
                                  ? trackValue.find("keys")
                                  : trackValue.end();
            if (!trackValue.is_object() ||
                !ReadString(trackValue, "id", track.id) ||
                !IsUuid(track.id) ||
                !identities.insert(track.id).second ||
                !ReadString(trackValue, "nodeId", track.nodeId) ||
                !nodes.contains(track.nodeId) ||
                !ReadString(trackValue, "property", track.property) ||
                !SupportedRuntimeProperty(track.property) ||
                keys == trackValue.end() || !keys->is_array() ||
                keys->empty()) {
                AddDiagnostic(result, "PXSDKUI1073",
                              "Animation track fields are invalid");
                continue;
            }
            float previous = -1.0f;
            for (const auto& keyValue : *keys) {
                UiAnimationKey key;
                std::string easing;
                std::string interpolation;
                const auto time = keyValue.is_object()
                                      ? keyValue.find("time")
                                      : keyValue.end();
                const auto authoredValue =
                    keyValue.is_object() ? keyValue.find("value")
                                         : keyValue.end();
                const auto parsedEase =
                    keyValue.is_object() &&
                            ReadString(keyValue, "easing", easing)
                        ? ParseAnimationEase(easing)
                        : std::nullopt;
                const auto parsedInterpolation =
                    keyValue.is_object() &&
                            ReadString(keyValue, "interpolation", interpolation)
                        ? ParseAnimationInterpolation(interpolation)
                        : std::nullopt;
                if (!keyValue.is_object() ||
                    !ReadString(keyValue, "id", key.id) ||
                    !IsUuid(key.id) ||
                    !identities.insert(key.id).second ||
                    time == keyValue.end() || !time->is_number() ||
                    !std::isfinite(key.time = time->get<float>()) ||
                    key.time < previous || key.time < 0.0f ||
                    key.time > clip.duration ||
                    authoredValue == keyValue.end() ||
                    !ParseValue(*authoredValue, key.value) ||
                    !RuntimePropertyValueMatches(track.property, key.value) ||
                    (!track.keys.empty() &&
                     !SameAnimationValueType(track.keys.front().value,
                                             key.value)) ||
                    !parsedEase || !parsedInterpolation) {
                    AddDiagnostic(result, "PXSDKUI1074",
                                  "Animation key fields or value type are invalid");
                    continue;
                }
                key.easing = *parsedEase;
                key.interpolation = *parsedInterpolation;
                previous = key.time;
                track.keys.push_back(std::move(key));
            }
            if (!track.keys.empty()) clip.tracks.push_back(std::move(track));
        }
        animations.clips.push_back(std::move(clip));
    }

    auto& stateMachine = animations.stateMachine;
    if (!ReadString(*machine, "entryStateId", stateMachine.entryStateId) ||
        !IsUuid(stateMachine.entryStateId)) {
        AddDiagnostic(result, "PXSDKUI1075",
                      "Animation stateMachine entryStateId must be a UUID");
    }
    const auto parameters = machine->find("parameters");
    const auto states = machine->find("states");
    const auto transitions = machine->find("transitions");
    if (parameters == machine->end() || !parameters->is_array() ||
        states == machine->end() || !states->is_array() ||
        transitions == machine->end() || !transitions->is_array()) {
        AddDiagnostic(
            result, "PXSDKUI1076",
            "Animation stateMachine requires parameters, states, and transitions");
        return;
    }

    std::unordered_map<std::string, UiAnimationParameterType>
        parameterTypes;
    for (const auto& item : *parameters) {
        UiAnimationParameter parameter;
        std::string type;
        const auto defaultValue =
            item.is_object() ? item.find("defaultValue") : item.end();
        const auto parsedType =
            item.is_object() && ReadString(item, "type", type)
                ? ParseAnimationParameterType(type)
                : std::nullopt;
        if (!item.is_object() || !ReadString(item, "id", parameter.id) ||
            !IsUuid(parameter.id) ||
            !identities.insert(parameter.id).second ||
            !ReadString(item, "name", parameter.name) ||
            !IsIdentifier(parameter.name) ||
            parameterTypes.contains(parameter.name) || !parsedType ||
            defaultValue == item.end() ||
            !ParseValue(*defaultValue, parameter.defaultValue) ||
            ((*parsedType == UiAnimationParameterType::Trigger ||
              *parsedType == UiAnimationParameterType::Bool) &&
             !std::holds_alternative<bool>(parameter.defaultValue)) ||
            (*parsedType == UiAnimationParameterType::Number &&
             !IsNumber(parameter.defaultValue))) {
            AddDiagnostic(result, "PXSDKUI1077",
                          "Animation parameter fields are invalid");
            continue;
        }
        parameter.type = *parsedType;
        parameterTypes.emplace(parameter.name, parameter.type);
        stateMachine.parameters.push_back(std::move(parameter));
    }

    std::unordered_set<std::string> stateIds;
    std::unordered_set<std::string> stateNames;
    for (const auto& item : *states) {
        UiAnimationState state;
        const auto position =
            item.is_object() ? item.find("position") : item.end();
        if (!item.is_object() || !ReadString(item, "id", state.id) ||
            !IsUuid(state.id) || !identities.insert(state.id).second ||
            !stateIds.insert(state.id).second ||
            !ReadString(item, "name", state.name) ||
            !stateNames.insert(state.name).second ||
            !ReadString(item, "clipId", state.clipId) ||
            !clipIds.contains(state.clipId) || position == item.end() ||
            !position->is_object() ||
            !ReadFiniteFloat(*position, "x", state.x) ||
            !ReadFiniteFloat(*position, "y", state.y)) {
            AddDiagnostic(result, "PXSDKUI1078",
                          "Animation state fields are invalid");
            continue;
        }
        stateMachine.states.push_back(std::move(state));
    }
    if (stateMachine.states.empty() ||
        !stateIds.contains(stateMachine.entryStateId))
        AddDiagnostic(result, "PXSDKUI1079",
                      "Animation stateMachine requires a valid entry state");

    for (const auto& item : *transitions) {
        UiAnimationTransition transition;
        const auto from =
            item.is_object() ? item.find("fromStateId") : item.end();
        const auto conditions =
            item.is_object() ? item.find("conditions") : item.end();
        const auto hasExitTime =
            item.is_object() ? item.find("hasExitTime") : item.end();
        const auto exitTime =
            item.is_object() ? item.find("exitTime") : item.end();
        const auto duration =
            item.is_object() ? item.find("duration") : item.end();
        const auto priority =
            item.is_object() ? item.find("priority") : item.end();
        if (!item.is_object() ||
            !ReadString(item, "id", transition.id) ||
            !IsUuid(transition.id) ||
            !identities.insert(transition.id).second ||
            from == item.end() ||
            (!from->is_null() && !from->is_string()) ||
            !ReadString(item, "toStateId", transition.toStateId) ||
            !stateIds.contains(transition.toStateId) ||
            conditions == item.end() || !conditions->is_array() ||
            hasExitTime == item.end() || !hasExitTime->is_boolean() ||
            exitTime == item.end() || !exitTime->is_number() ||
            duration == item.end() || !duration->is_number() ||
            priority == item.end() || !priority->is_number_integer()) {
            AddDiagnostic(result, "PXSDKUI1080",
                          "Animation transition fields are invalid");
            continue;
        }
        if (from->is_string()) {
            transition.fromStateId = from->get<std::string>();
            if (!IsUuid(*transition.fromStateId) ||
                !stateIds.contains(*transition.fromStateId)) {
                AddDiagnostic(result, "PXSDKUI1080",
                              "Animation transition source state is invalid");
                continue;
            }
        }
        transition.hasExitTime = hasExitTime->get<bool>();
        transition.exitTime = exitTime->get<float>();
        transition.duration = duration->get<float>();
        transition.priority = priority->get<int>();
        if (!std::isfinite(transition.exitTime) ||
            !std::isfinite(transition.duration) ||
            transition.exitTime < 0.0f || transition.exitTime > 1.0f ||
            transition.duration < 0.0f) {
            AddDiagnostic(result, "PXSDKUI1080",
                          "Animation transition timing is invalid");
            continue;
        }
        for (const auto& conditionValue : *conditions) {
            UiAnimationCondition condition;
            std::string operation;
            const auto expected = conditionValue.is_object()
                                      ? conditionValue.find("value")
                                      : conditionValue.end();
            const auto parsedOperation =
                conditionValue.is_object() &&
                        ReadString(conditionValue, "operator", operation)
                    ? ParseAnimationCondition(operation)
                    : std::nullopt;
            if (!conditionValue.is_object() ||
                !ReadString(conditionValue, "id", condition.id) ||
                !IsUuid(condition.id) ||
                !identities.insert(condition.id).second ||
                !ReadString(conditionValue, "parameter",
                            condition.parameter) ||
                !parameterTypes.contains(condition.parameter) ||
                !parsedOperation || expected == conditionValue.end() ||
                !ParseValue(*expected, condition.value)) {
                AddDiagnostic(result, "PXSDKUI1081",
                              "Animation condition fields are invalid");
                continue;
            }
            const auto type = parameterTypes.at(condition.parameter);
            const bool valid =
                (type == UiAnimationParameterType::Trigger &&
                 *parsedOperation ==
                     UiAnimationConditionOperator::Triggered &&
                 std::holds_alternative<bool>(condition.value)) ||
                (type == UiAnimationParameterType::Bool &&
                 (*parsedOperation ==
                      UiAnimationConditionOperator::Equal ||
                  *parsedOperation ==
                      UiAnimationConditionOperator::NotEqual) &&
                 std::holds_alternative<bool>(condition.value)) ||
                (type == UiAnimationParameterType::Number &&
                 *parsedOperation !=
                     UiAnimationConditionOperator::Triggered &&
                 IsNumber(condition.value));
            if (!valid) {
                AddDiagnostic(result, "PXSDKUI1082",
                              "Animation condition does not match its parameter");
                continue;
            }
            condition.operation = *parsedOperation;
            transition.conditions.push_back(std::move(condition));
        }
        stateMachine.transitions.push_back(std::move(transition));
    }
    result.document.animations = std::move(animations);
}

void ValidateBehaviorAnimationReferences(UiParseResult& result) {
    using Kind = UiBehaviorNodeKind;
    std::unordered_set<std::string> stateNames;
    std::unordered_set<std::string> clipIds;
    std::unordered_map<std::string, UiAnimationParameterType> parameters;
    if (result.document.animations) {
        for (const auto& clip : result.document.animations->clips)
            clipIds.insert(clip.id);
        for (const auto& state :
             result.document.animations->stateMachine.states)
            stateNames.insert(state.name);
        for (const auto& parameter :
             result.document.animations->stateMachine.parameters)
            parameters.emplace(parameter.name, parameter.type);
    }
    for (const auto& group : result.document.visualStateGroups)
        for (const auto& transition : group.transitions)
            if (transition.animationClipId &&
                !clipIds.contains(*transition.animationClipId))
                AddDiagnostic(
                    result, "PXSDKUI1087",
                    "Visual State transition animationClipId is unresolved");
    for (const auto& node : result.document.behaviorGraph.nodes) {
        if (node.kind == Kind::PlayAnimation ||
            node.kind == Kind::TravelAnimationState) {
            const char* property =
                node.kind == Kind::PlayAnimation ? "name" : "state";
            const auto* name = String(Property(node, property));
            if (!result.document.animations || !name ||
                (*name != "default" && *name != "embedded" &&
                 !stateNames.contains(*name)))
                AddDiagnostic(
                    result, "PXSDKUI1085",
                    "Behavior animation state must exist in this UI document");
        } else if (node.kind == Kind::SetAnimationParameter) {
            const auto* name = String(Property(node, "name"));
            const auto found =
                name ? parameters.find(*name) : parameters.end();
            const auto* value = Property(node, "value");
            const bool matches =
                found != parameters.end() && value &&
                ((found->second == UiAnimationParameterType::Trigger) ||
                 (found->second == UiAnimationParameterType::Bool &&
                  std::holds_alternative<bool>(*value)) ||
                 (found->second == UiAnimationParameterType::Number &&
                  IsNumber(*value)));
            if (!matches)
                AddDiagnostic(
                    result, "PXSDKUI1086",
                    "Behavior animation parameter must exist with a matching type");
        }
    }
}

}  // namespace

UiParseResult ParseUi(const std::string_view text) {
    UiParseResult result;
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        AddDiagnostic(result, "PXSDKUI1001", "UI document must be a JSON object");
        return result;
    }
    if (root.value("format", std::string{}) != "PrismatiXUIScene")
        AddDiagnostic(result, "PXSDKUI1002", "UI document format is not PrismatiXUIScene");
    const auto schemaRevision = root.find("schemaRevision");
    if (schemaRevision == root.end() || !schemaRevision->is_number_unsigned() ||
        (schemaRevision->get<std::uint32_t>() != 1 &&
         schemaRevision->get<std::uint32_t>() != 2))
        AddDiagnostic(result, "PXSDKUI1003", "Unsupported UI document schema revision");
    else
        result.document.schemaRevision = schemaRevision->get<std::uint32_t>();
    if (!ReadString(root, "id", result.document.id) || !IsUuid(result.document.id))
        AddDiagnostic(result, "PXSDKUI1004", "UI document id must be a UUID");
    const auto revision = root.find("revision");
    if (revision == root.end() || !revision->is_number_unsigned())
        AddDiagnostic(result, "PXSDKUI1005", "UI document revision is required");
    else result.document.revision = revision->get<std::uint64_t>();
    if (!ReadString(root, "name", result.document.name))
        AddDiagnostic(result, "PXSDKUI1006", "UI document name is required");
    const auto width = root.find("width"), height = root.find("height");
    if (width == root.end() || !width->is_number_unsigned() || width->get<std::uint32_t>() == 0 ||
        height == root.end() || !height->is_number_unsigned() || height->get<std::uint32_t>() == 0) {
        AddDiagnostic(result, "PXSDKUI1007", "UI document artboard size must be positive");
    } else {
        result.document.width = width->get<std::uint32_t>();
        result.document.height = height->get<std::uint32_t>();
    }
    if (!ReadString(root, "rootId", result.document.rootId) || !IsUuid(result.document.rootId))
        AddDiagnostic(result, "PXSDKUI1008", "UI document rootId must be a UUID");

    const auto nodes = root.find("nodes");
    if (nodes == root.end() || !nodes->is_array()) {
        AddDiagnostic(result, "PXSDKUI1009", "UI document nodes must be an array");
        return result;
    }
    std::unordered_set<std::string> identities;
    std::unordered_set<std::string> siblingOrders;
    result.document.nodes.reserve(nodes->size());
    for (std::size_t index = 0; index < nodes->size(); ++index) {
        const Json& item = (*nodes)[index];
        if (!item.is_object()) {
            AddDiagnostic(result, "PXSDKUI1010", "UI node must be an object", index);
            continue;
        }
        UiNode node;
        bool valid = true;
        if (!ReadString(item, "id", node.id) || !IsUuid(node.id) ||
            !identities.insert(node.id).second) {
            AddDiagnostic(result, "PXSDKUI1011", "UI node id must be a unique UUID", index);
            valid = false;
        }
        const auto parentId = item.find("parentId");
        if (parentId == item.end() || (!parentId->is_null() && !parentId->is_string())) {
            AddDiagnostic(result, "PXSDKUI1012", "UI node parentId must be a UUID or null", index);
            valid = false;
        } else if (parentId->is_string()) {
            node.parentId = parentId->get<std::string>();
            if (!IsUuid(*node.parentId)) {
                AddDiagnostic(result, "PXSDKUI1012", "UI node parentId must be a UUID or null", index);
                valid = false;
            }
        }
        const auto order = item.find("order");
        if (order == item.end() || !order->is_number_unsigned()) {
            AddDiagnostic(result, "PXSDKUI1013", "UI node order must be unsigned", index);
            valid = false;
        } else node.order = order->get<std::uint32_t>();
        const std::string siblingKey = node.parentId.value_or("<root>") + ":" + std::to_string(node.order);
        if (!siblingOrders.insert(siblingKey).second) {
            AddDiagnostic(result, "PXSDKUI1014", "UI sibling order must be unique", index);
            valid = false;
        }
        std::string kind;
        const auto parsedKind = ReadString(item, "kind", kind)
                                    ? ParseKind(kind)
                                    : std::nullopt;
        if (!parsedKind) {
            AddDiagnostic(result, "PXSDKUI1015", "UI node kind is unsupported", index);
            valid = false;
        } else node.kind = *parsedKind;
        const auto runtimeType = item.find("runtimeType");
        if (result.document.schemaRevision == 1) {
            if (runtimeType != item.end()) {
                AddDiagnostic(result, "PXSDKUI1018",
                              "Revision-1 UI nodes cannot declare runtimeType",
                              index);
                valid = false;
            }
        } else if (runtimeType != item.end()) {
            std::string value;
            if (!runtimeType->is_string() ||
                !ReadString(item, "runtimeType", value) ||
                !std::all_of(value.begin(), value.end(), [](const char byte) {
                    const auto character = static_cast<unsigned char>(byte);
                    return std::isalnum(character) || byte == '_' ||
                           byte == '-' || byte == '.';
                })) {
                AddDiagnostic(result, "PXSDKUI1018",
                              "UI node runtimeType is invalid", index);
                valid = false;
            } else {
                node.runtimeType = std::move(value);
            }
        }
        if (node.kind == UiNodeKind::Leaf && !node.runtimeType) {
            AddDiagnostic(result, "PXSDKUI1018",
                          "UI leaf nodes require a revision-2 runtimeType",
                          index);
            valid = false;
        }
        if (!ReadString(item, "name", node.name)) {
            AddDiagnostic(result, "PXSDKUI1016", "UI node name is required", index);
            valid = false;
        }
        const auto visible = item.find("visible"), locked = item.find("locked");
        if (visible == item.end() || !visible->is_boolean() ||
            locked == item.end() || !locked->is_boolean()) {
            AddDiagnostic(result, "PXSDKUI1017", "UI node visibility and lock state are required", index);
            valid = false;
        } else {
            node.visible = visible->get<bool>();
            node.locked = locked->get<bool>();
        }

        const auto layout = item.find("layout");
        if (layout == item.end() || !layout->is_object()) {
            AddDiagnostic(result, "PXSDKUI1020", "UI node layout is required", index);
            valid = false;
        } else {
            bool layoutValid = true;
            std::string mode;
            if (!ReadString(*layout, "mode", mode) || (mode != "free" && mode != "container")) layoutValid = false;
            else node.layout.mode = mode == "free" ? UiLayoutMode::Free : UiLayoutMode::Container;
            layoutValid = ReadFiniteFloat(*layout, "x", node.layout.x) &&
                          ReadFiniteFloat(*layout, "y", node.layout.y) &&
                          ReadFiniteFloat(*layout, "width", node.layout.width) &&
                          ReadFiniteFloat(*layout, "height", node.layout.height) &&
                          ReadFiniteFloat(*layout, "anchorX", node.layout.anchorX) &&
                          ReadFiniteFloat(*layout, "anchorY", node.layout.anchorY) &&
                          ReadFiniteFloat(*layout, "pivotX", node.layout.pivotX) &&
                          ReadFiniteFloat(*layout, "pivotY", node.layout.pivotY) &&
                          ReadFiniteFloat(*layout, "margin", node.layout.margin) &&
                          layoutValid;
            layoutValid = ReadString(*layout, "alignment", node.layout.alignment) &&
                          ReadString(*layout, "sizeRule", node.layout.sizeRule) &&
                          layoutValid;
            node.layout.anchorRight = node.layout.anchorX;
            node.layout.anchorBottom = node.layout.anchorY;
            const auto anchorRight = layout->find("anchorRight");
            const auto anchorBottom = layout->find("anchorBottom");
            if (anchorRight != layout->end()) {
                layoutValid = result.document.schemaRevision == 2 &&
                              ReadFiniteFloat(*layout, "anchorRight",
                                              node.layout.anchorRight) &&
                              layoutValid;
            }
            if (anchorBottom != layout->end()) {
                layoutValid = result.document.schemaRevision == 2 &&
                              ReadFiniteFloat(*layout, "anchorBottom",
                                              node.layout.anchorBottom) &&
                              layoutValid;
            }
            if (!layoutValid || node.layout.width <= 0.0f || node.layout.height <= 0.0f ||
                node.layout.anchorX < 0.0f || node.layout.anchorX > 1.0f ||
                node.layout.anchorY < 0.0f || node.layout.anchorY > 1.0f ||
                node.layout.anchorRight < node.layout.anchorX ||
                node.layout.anchorRight > 1.0f ||
                node.layout.anchorBottom < node.layout.anchorY ||
                node.layout.anchorBottom > 1.0f ||
                node.layout.pivotX < 0.0f || node.layout.pivotX > 1.0f ||
                node.layout.pivotY < 0.0f || node.layout.pivotY > 1.0f ||
                (node.layout.alignment != "start" && node.layout.alignment != "center" &&
                 node.layout.alignment != "end" && node.layout.alignment != "stretch") ||
                (node.layout.sizeRule != "fixed" && node.layout.sizeRule != "fill" &&
                 node.layout.sizeRule != "content")) {
                AddDiagnostic(result, "PXSDKUI1021", "UI node layout is invalid", index);
                valid = false;
            }
        }

        const auto content = item.find("content");
        if (content == item.end() || !content->is_object() ||
            !ReadString(*content, "text", node.text, true)) {
            AddDiagnostic(result, "PXSDKUI1022", "UI node content is invalid", index);
            valid = false;
        } else {
            const auto assetId = content->find("assetId");
            if (assetId == content->end() || (!assetId->is_null() && !assetId->is_string())) {
                AddDiagnostic(result, "PXSDKUI1022", "UI node assetId must be a UUID or null", index);
                valid = false;
            } else if (assetId->is_string()) {
                node.assetId = assetId->get<std::string>();
                if (!IsUuid(*node.assetId) || node.kind != UiNodeKind::Image) {
                    AddDiagnostic(result, "PXSDKUI1022", "Only image nodes may reference an asset UUID", index);
                    valid = false;
                }
            }
        }

        const auto appearance = item.find("appearance");
        if (appearance == item.end() || !appearance->is_object() ||
            !ReadString(*appearance, "backgroundColor", node.appearance.backgroundColor) ||
            !ReadString(*appearance, "textColor", node.appearance.textColor) ||
            !ReadFiniteFloat(*appearance, "opacity", node.appearance.opacity) ||
            !ReadFiniteFloat(*appearance, "disabledOpacity", node.appearance.disabledOpacity)) {
            AddDiagnostic(result, "PXSDKUI1023", "UI node appearance is invalid", index);
            valid = false;
        } else {
            bool appearanceValid = true;
            const auto readOptionalColor = [&](const char* key, std::optional<std::string>& output) {
                const auto found = appearance->find(key);
                if (found == appearance->end() || found->is_null()) return true;
                if (!found->is_string()) return false;
                output = found->get<std::string>();
                return IsColor(*output);
            };
            const auto styleToken = appearance->find("styleToken");
            if (styleToken == appearance->end() ||
                (!styleToken->is_null() && !styleToken->is_string())) appearanceValid = false;
            else if (styleToken->is_string()) node.appearance.styleToken = styleToken->get<std::string>();
            if (!IsColor(node.appearance.backgroundColor) || !IsColor(node.appearance.textColor) ||
                node.appearance.opacity < 0.0f || node.appearance.opacity > 1.0f ||
                node.appearance.disabledOpacity < 0.0f || node.appearance.disabledOpacity > 1.0f ||
                !readOptionalColor("hoverBackgroundColor", node.appearance.hoverBackgroundColor) ||
                !readOptionalColor("focusColor", node.appearance.focusColor)) appearanceValid = false;
            if (!appearanceValid) {
                AddDiagnostic(result, "PXSDKUI1023", "UI node appearance is invalid", index);
                valid = false;
            }
        }

        const auto interaction = item.find("interaction");
        if (interaction == item.end() || !interaction->is_object()) {
            AddDiagnostic(result, "PXSDKUI1024", "UI node interaction is required", index);
            valid = false;
        } else {
            const auto onClick = interaction->find("onClick");
            if (onClick == interaction->end() || (!onClick->is_null() && !onClick->is_object())) {
                AddDiagnostic(result, "PXSDKUI1024", "UI node onClick must be an action or null", index);
                valid = false;
            } else if (onClick->is_object()) {
                UiAction action;
                bool actionValid =
                                   (node.kind == UiNodeKind::Button ||
                                    node.runtimeType.has_value()) &&
                                   ReadString(*onClick, "id", action.id);
                const auto arguments = onClick->find("arguments");
                if (arguments == onClick->end() || !arguments->is_object()) actionValid = false;
                else for (auto argument = arguments->begin(); argument != arguments->end(); ++argument) {
                    UiActionValue value;
                    if (!ParseActionValue(argument.value(), value)) actionValid = false;
                    else action.arguments.emplace(argument.key(), std::move(value));
                }
                actionValid = actionValid && ValidAction(action);
                if (!actionValid) {
                    AddDiagnostic(result, "PXSDKUI1025", "UI node typed action is invalid", index);
                    valid = false;
                } else node.onClick = std::move(action);
            }
        }

        const auto accessibility = item.find("accessibility");
        if (accessibility == item.end() || !accessibility->is_object() ||
            !ReadString(*accessibility, "label", node.accessibilityLabel, true) ||
            !ReadString(*accessibility, "role", node.accessibilityRole)) {
            AddDiagnostic(result, "PXSDKUI1026", "UI node accessibility is invalid", index);
            valid = false;
        }
        const auto runtimeProperties = item.find("runtimeProperties");
        if (runtimeProperties != item.end() &&
            (!ReadValueMap(*runtimeProperties, node.runtimeProperties) ||
             node.runtimeProperties.size() > 256)) {
            AddDiagnostic(
                result, "PXSDKUI1028",
                "UI node runtimeProperties must be a bounded typed value map",
                index);
            valid = false;
        }
        const auto bindings = item.find("bindings");
        if (bindings != item.end()) {
            bool bindingsValid = result.document.schemaRevision == 2 &&
                                 bindings->is_object() &&
                                 bindings->size() <= 256;
            if (bindings->is_object() && bindings->size() <= 256) {
                for (auto binding = bindings->begin(); binding != bindings->end();
                     ++binding) {
                    UiPropertyBinding descriptor;
                    bool descriptorValid = IsIdentifier(binding.key()) &&
                                           binding.value().is_object() &&
                                           (binding.value().size() == 1 ||
                                            binding.value().size() == 2) &&
                                           ReadString(binding.value(), "path",
                                                      descriptor.path) &&
                                           IsPropertyPath(descriptor.path);
                    const auto formatter = binding.value().find("formatter");
                    descriptorValid = binding.value().size() ==
                                              (formatter != binding.value().end() ? 2u : 1u) &&
                                      descriptorValid;
                    if (formatter != binding.value().end()) {
                        descriptorValid =
                            formatter->is_string() &&
                            formatter->get_ref<const std::string&>().size() <= 128 &&
                            IsIdentifier(formatter->get_ref<const std::string&>()) &&
                            descriptorValid;
                        if (formatter->is_string())
                            descriptor.formatter = formatter->get<std::string>();
                    }
                    bindingsValid = bindingsValid && descriptorValid;
                    if (descriptorValid)
                        node.bindings.emplace(binding.key(),
                                              std::move(descriptor));
                }
            }
            if (!bindingsValid) {
                AddDiagnostic(
                    result, "PXSDKUI1065",
                    "Revision-2 UI property bindings require bounded typed paths",
                    index);
                valid = false;
            }
        }
        const auto componentInstance = item.find("componentInstance");
        if (componentInstance != item.end() && !componentInstance->is_null()) {
            UiComponentInstance instance;
            static const std::unordered_set<std::string> overridePaths{
                "name", "visible", "locked", "layout.x", "layout.y",
                "layout.width", "layout.height", "layout.anchorX",
                "layout.anchorY", "layout.anchorRight", "layout.anchorBottom",
                "layout.pivotX", "layout.pivotY",
                "layout.margin", "layout.alignment", "layout.sizeRule",
                "content.text", "content.assetId",
                "appearance.backgroundColor", "appearance.textColor",
                "appearance.opacity", "appearance.styleToken",
                "appearance.hoverBackgroundColor", "appearance.focusColor",
                "appearance.disabledOpacity", "interaction.onClick",
                "accessibility.label", "accessibility.role"};
            bool instanceValid = componentInstance->is_object() &&
                                 ReadString(*componentInstance, "componentId",
                                            instance.componentId) &&
                                 IsUuid(instance.componentId) &&
                                 ReadString(*componentInstance, "instanceRootId",
                                            instance.instanceRootId) &&
                                 IsUuid(instance.instanceRootId) &&
                                 ReadString(*componentInstance, "sourceNodeId",
                                            instance.sourceNodeId) &&
                                 IsUuid(instance.sourceNodeId);
            const auto overrides = componentInstance->find("overrides");
            std::unordered_set<std::string> uniqueOverrides;
            if (overrides == componentInstance->end() ||
                !overrides->is_array()) {
                instanceValid = false;
            } else {
                for (const auto& path : *overrides) {
                    const std::string pathText =
                        path.is_string() ? path.get<std::string>()
                                         : std::string{};
                    const bool runtimeProperty =
                        pathText.starts_with("runtimeProperties.") &&
                        pathText.size() > std::string_view(
                                              "runtimeProperties.")
                                              .size();
                    if (!path.is_string() ||
                        (!overridePaths.contains(pathText) &&
                         !runtimeProperty) ||
                        !uniqueOverrides.insert(pathText).second) {
                        instanceValid = false;
                        continue;
                    }
                    instance.overrides.push_back(pathText);
                }
            }
            const auto sourcePath = componentInstance->find("sourcePath");
            if (sourcePath != componentInstance->end()) {
                if (!sourcePath->is_array() || sourcePath->size() < 2) {
                    instanceValid = false;
                } else {
                    for (const auto& identity : *sourcePath) {
                        if (!identity.is_string() || identity.empty() ||
                            !IsUuid(identity.get_ref<const std::string&>())) {
                            instanceValid = false;
                            continue;
                        }
                        instance.sourcePath.push_back(
                            identity.get<std::string>());
                    }
                }
            }
            const auto publicProperties =
                componentInstance->find("publicProperties");
            if (publicProperties != componentInstance->end()) {
                if (!publicProperties->is_object() ||
                    publicProperties->size() > 256) {
                    instanceValid = false;
                } else {
                    for (auto property = publicProperties->begin();
                         property != publicProperties->end(); ++property) {
                        if (property.key().empty() ||
                            !IsJsonValue(property.value())) {
                            instanceValid = false;
                            continue;
                        }
                        instance.publicProperties.emplace(
                            property.key(),
                            UiComponentPublicValue{
                                property.value().dump()});
                    }
                }
            }
            const auto publicSignals =
                componentInstance->find("publicSignals");
            if (publicSignals != componentInstance->end()) {
                if (!publicSignals->is_object() || publicSignals->size() > 256) {
                    instanceValid = false;
                } else {
                    for (auto signal = publicSignals->begin();
                         signal != publicSignals->end(); ++signal) {
                        std::optional<UiComponentSignalBinding> binding;
                        if (signal.key().empty() ||
                            !ParsePublicSignalBinding(
                                signal.value(), result.document.schemaRevision,
                                binding)) {
                            AddDiagnostic(
                                result, "PXSDKUI1029",
                                "UI component public signal binding requires a "
                                "typed Action with Runtime-representable arguments",
                                index);
                            instanceValid = false;
                            continue;
                        }
                        instance.publicSignals.emplace(signal.key(),
                                                       std::move(binding));
                    }
                }
            }
            if (!instanceValid) {
                AddDiagnostic(result, "PXSDKUI1027",
                              "UI node component instance metadata is invalid",
                              index);
                valid = false;
            } else {
                node.componentInstance = std::move(instance);
            }
        }
        const auto componentSlot = item.find("componentSlot");
        if (componentSlot != item.end() && !componentSlot->is_null()) {
            UiComponentSlot slot;
            const bool slotValid =
                componentSlot->is_object() && componentSlot->size() == 2 &&
                ReadString(*componentSlot, "instanceRootId",
                           slot.instanceRootId) &&
                IsUuid(slot.instanceRootId) &&
                ReadString(*componentSlot, "slotId", slot.slotId);
            if (!slotValid) {
                AddDiagnostic(result, "PXSDKUI1035",
                              "UI node component slot metadata is invalid",
                              index);
                valid = false;
            } else {
                node.componentSlot = std::move(slot);
            }
        }
        if (valid) result.document.nodes.push_back(std::move(node));
    }

    const auto theme = root.find("theme");
    if (theme == root.end() || !theme->is_array()) {
        AddDiagnostic(result, "PXSDKUI1030", "UI document theme must be an array");
    } else {
        std::unordered_set<std::string> names;
        for (const auto& item : *theme) {
            UiThemeToken token;
            if (!item.is_object() || !ReadString(item, "id", token.id) || !IsUuid(token.id) ||
                !identities.insert(token.id).second || !ReadString(item, "name", token.name) ||
                !IsIdentifier(token.name) || !names.insert(token.name).second ||
                !ReadString(item, "value", token.value)) {
                AddDiagnostic(result, "PXSDKUI1031", "UI theme tokens require unique UUIDs, identifiers, and values");
                continue;
            }
            result.document.theme.push_back(std::move(token));
        }
        for (const auto& node : result.document.nodes) {
            if (node.appearance.styleToken && !names.contains(*node.appearance.styleToken))
                AddDiagnostic(result, "PXSDKUI1032", "UI node references a missing style token");
        }
    }

    std::unordered_map<std::string, const UiNode*> byId;
    for (const auto& node : result.document.nodes) byId.emplace(node.id, &node);
    std::unordered_set<std::string> componentSources;
    for (const auto& node : result.document.nodes) {
        if (!node.componentInstance) continue;
        const auto& instance = *node.componentInstance;
        const auto instanceRoot = byId.find(instance.instanceRootId);
        if (instanceRoot == byId.end() ||
            !instanceRoot->second->componentInstance ||
            instanceRoot->second->componentInstance->componentId !=
                instance.componentId ||
            instanceRoot->second->componentInstance->instanceRootId !=
                instance.instanceRootId ||
            !componentSources
                 .insert(instance.instanceRootId + ":" + instance.sourceNodeId)
                 .second) {
            AddDiagnostic(
                result, "PXSDKUI1033",
                "UI component instances require one root and unique source nodes");
            continue;
        }
        bool inside = false;
        const UiNode* cursor = &node;
        while (cursor) {
            if (cursor->id == instance.instanceRootId) {
                inside = true;
                break;
            }
            if (!cursor->parentId) break;
            const auto parent = byId.find(*cursor->parentId);
            cursor = parent == byId.end() ? nullptr : parent->second;
        }
        if (!inside)
            AddDiagnostic(result, "PXSDKUI1034",
                          "UI node crosses its component instance boundary");
        if (node.id != instance.instanceRootId &&
            (!instance.publicProperties.empty() ||
             !instance.publicSignals.empty())) {
            AddDiagnostic(result, "PXSDKUI1036",
                          "Only a component instance root may own public values");
        }
        const bool directSourcePath =
            instance.sourcePath.size() == 2 &&
            instance.sourcePath.front() == instance.componentId &&
            instance.sourcePath.back() == instance.sourceNodeId;
        const bool nestedSourcePath =
            instance.sourcePath.size() == 4 &&
            instance.sourcePath.front() == instance.componentId;
        if (!instance.sourcePath.empty() && !directSourcePath &&
            !nestedSourcePath) {
            AddDiagnostic(result, "PXSDKUI1037",
                          "UI component sourcePath does not match its stable identities");
        }
    }
    for (const auto& node : result.document.nodes) {
        if (!node.componentSlot) continue;
        const auto instanceRoot = byId.find(node.componentSlot->instanceRootId);
        const auto parent = node.parentId ? byId.find(*node.parentId) : byId.end();
        if ((node.componentInstance &&
             node.id != node.componentInstance->instanceRootId) ||
            instanceRoot == byId.end() ||
            !instanceRoot->second->componentInstance ||
            instanceRoot->second->componentInstance->instanceRootId !=
                instanceRoot->second->id ||
            parent == byId.end() || !parent->second->componentInstance ||
            parent->second->componentInstance->instanceRootId !=
                node.componentSlot->instanceRootId) {
            AddDiagnostic(
                result, "PXSDKUI1038",
                "UI component slot content must be local content inside one instance");
        }
    }
    for (const auto& node : result.document.nodes) {
        if (node.componentInstance || node.componentSlot || !node.parentId) continue;
        const UiNode* cursor = nullptr;
        if (const auto parent = byId.find(*node.parentId); parent != byId.end())
            cursor = parent->second;
        std::unordered_set<std::string> visited;
        while (cursor && visited.insert(cursor->id).second) {
            // A nested component root owns a new immutable source boundary even
            // when it is itself projected through an outer named slot.
            if (cursor->componentInstance) {
                AddDiagnostic(
                    result, "PXSDKUI1039",
                    "Local UI content inside a component instance requires a named slot");
                break;
            }
            if (cursor->componentSlot) break;
            if (!cursor->parentId) break;
            const auto parent = byId.find(*cursor->parentId);
            cursor = parent == byId.end() ? nullptr : parent->second;
        }
    }
    ParseBehaviorSections(root, result, identities, byId);
    ParseVisualStateSection(root, result, byId);
    ParseAnimationSection(root, result, identities, byId);
    ValidateBehaviorAnimationReferences(result);
    const auto rootNode = byId.find(result.document.rootId);
    if (rootNode == byId.end() || rootNode->second->parentId ||
        rootNode->second->kind != UiNodeKind::Control) {
        AddDiagnostic(result, "PXSDKUI1040", "UI document root must be a parentless control");
    }
    if (std::count_if(result.document.nodes.begin(), result.document.nodes.end(),
                      [](const UiNode& node) { return !node.parentId; }) != 1)
        AddDiagnostic(result, "PXSDKUI1041", "UI document requires exactly one root");
    for (const auto& node : result.document.nodes) {
        if (!node.parentId) continue;
        const auto parent = byId.find(*node.parentId);
        if (parent == byId.end()) {
            AddDiagnostic(result, "PXSDKUI1042", "UI node references a missing parent");
            continue;
        }
        if (OwnsLayout(parent->second->kind) && node.layout.mode != UiLayoutMode::Container)
            AddDiagnostic(result, "PXSDKUI1043", "UI node layout must be container-owned");
        std::unordered_set<std::string> visited;
        auto cursor = node.parentId;
        while (cursor) {
            if (*cursor == node.id || !visited.insert(*cursor).second) {
                AddDiagnostic(result, "PXSDKUI1044", "UI hierarchy cannot contain a cycle");
                break;
            }
            const auto found = byId.find(*cursor);
            cursor = found == byId.end() ? std::nullopt : found->second->parentId;
        }
    }
    return result;
}

UiComponentParseResult ParseUiComponent(
    const std::string_view text) {
    UiComponentParseResult result;
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        AddDiagnostic(result, "PXSDKUICOMP1101",
                      "UI component must be a JSON object");
        return result;
    }
    if (root.value("format", std::string{}) != "PrismatiXUIComponent") {
        AddDiagnostic(result, "PXSDKUICOMP1102",
                      "UI component format is not PrismatiXUIComponent");
        return result;
    }
    for (const char* sceneOnly :
         {"behaviorGraph", "behaviorTriggers", "animations"}) {
        if (root.contains(sceneOnly))
            AddDiagnostic(result, "PXSDKUICOMP1103",
                          std::string("UI component contains scene-only field: ") +
                              sceneOnly);
    }

    Json scene = root;
    scene["format"] = "PrismatiXUIScene";
    const UiParseResult parsed = ParseUi(scene.dump());
    result.document.content = parsed.document;
    for (const auto& diagnostic : parsed.diagnostics) {
        // A reusable component may use any structural control/container as its
        // root. The stricter scene contract requires a Control root because it
        // owns the viewport, which does not apply to component sources.
        if (diagnostic.code != "PXSDKUI1040")
            result.diagnostics.push_back(diagnostic);
    }
    const auto componentRoot = std::ranges::find_if(
        result.document.content.nodes, [&](const UiNode& node) {
            return node.id == result.document.content.rootId;
        });
    if (componentRoot == result.document.content.nodes.end() ||
        componentRoot->parentId ||
        componentRoot->kind == UiNodeKind::Button ||
        componentRoot->kind == UiNodeKind::Label ||
        componentRoot->kind == UiNodeKind::Image ||
        componentRoot->kind == UiNodeKind::Leaf) {
        AddDiagnostic(result, "PXSDKUICOMP1108",
                      "UI component root must be a parentless structural node");
    }

    const auto parseValueType = [](const std::string_view name)
        -> std::optional<UiComponentValueType> {
        if (name == "null") return UiComponentValueType::Null;
        if (name == "boolean") return UiComponentValueType::Boolean;
        if (name == "integer") return UiComponentValueType::Integer;
        if (name == "number") return UiComponentValueType::Number;
        if (name == "string") return UiComponentValueType::String;
        if (name == "vec2") return UiComponentValueType::Vec2;
        if (name == "rect") return UiComponentValueType::Rect;
        if (name == "color") return UiComponentValueType::Color;
        if (name == "uuid") return UiComponentValueType::Uuid;
        if (name == "resource") return UiComponentValueType::Resource;
        if (name == "token") return UiComponentValueType::Token;
        if (name == "array") return UiComponentValueType::Array;
        if (name == "object") return UiComponentValueType::Object;
        return std::nullopt;
    };
    const auto valueMatches = [](const UiComponentValueType type,
                                 const Json& value) {
        switch (type) {
            case UiComponentValueType::Null: return value.is_null();
            case UiComponentValueType::Boolean: return value.is_boolean();
            case UiComponentValueType::Integer:
                return value.is_number_integer();
            case UiComponentValueType::Number:
                return value.is_number() &&
                       (!value.is_number_float() ||
                        std::isfinite(value.get<double>()));
            case UiComponentValueType::String: return value.is_string();
            case UiComponentValueType::Uuid:
                return value.is_string() &&
                       IsUuid(value.get_ref<const std::string&>());
            case UiComponentValueType::Resource:
                if (value.is_string())
                    return value.get_ref<const std::string&>().empty() ||
                           IsUuid(value.get_ref<const std::string&>());
                else {
                    UiValue parsed;
                    return ParseValue(value, parsed) &&
                           std::holds_alternative<UiResourceValue>(parsed);
                }
            case UiComponentValueType::Token:
                if (value.is_string())
                    return !value.get_ref<const std::string&>().empty();
                else {
                    UiValue parsed;
                    return ParseValue(value, parsed) &&
                           std::holds_alternative<UiTokenValue>(parsed);
                }
            case UiComponentValueType::Array:
                return value.is_array() && IsJsonValue(value);
            case UiComponentValueType::Object:
                return value.is_object() && IsJsonValue(value);
            case UiComponentValueType::Vec2: {
                UiValue ignored;
                return ParseValue(value, ignored) &&
                       std::holds_alternative<UiVec2Value>(ignored);
            }
            case UiComponentValueType::Rect: {
                UiValue ignored;
                return ParseValue(value, ignored) &&
                       std::holds_alternative<UiRectValue>(ignored);
            }
            case UiComponentValueType::Color: {
                UiValue ignored;
                return ParseValue(value, ignored) &&
                       std::holds_alternative<UiColorValue>(ignored);
            }
        }
        return false;
    };
    const auto metadataValid = [](const Json& owner) {
        const auto metadata = owner.find("metadata");
        return metadata == owner.end() ||
               (metadata->is_object() && IsJsonValue(*metadata));
    };
    const auto interface = root.find("componentInterface");
    if (interface == root.end()) return result;
    if (!interface->is_object() || !metadataValid(*interface)) {
        AddDiagnostic(result, "PXSDKUICOMP1104",
                      "UI component interface is malformed");
        return result;
    }
    const auto properties = interface->find("properties");
    const auto signals = interface->find("signals");
    const auto slots = interface->find("slots");
    if (properties == interface->end() || !properties->is_array() ||
        signals == interface->end() || !signals->is_array() ||
        slots == interface->end() || !slots->is_array()) {
        AddDiagnostic(result, "PXSDKUICOMP1104",
                      "UI component interface requires properties, signals and slots arrays");
        return result;
    }
    std::unordered_map<std::string, const Json*> sourceNodes;
    if (const auto nodes = root.find("nodes");
        nodes != root.end() && nodes->is_array()) {
        for (const auto& node : *nodes) {
            const auto id = node.find("id");
            if (node.is_object() && id != node.end() && id->is_string())
                sourceNodes.emplace(id->get<std::string>(), &node);
        }
    }
    const auto propertyAtPath = [](const Json& node,
                                   const std::string_view path)
        -> const Json* {
        const Json* current = &node;
        std::size_t start = 0;
        while (start < path.size()) {
            const std::size_t end = path.find('.', start);
            const std::string key(path.substr(
                start, end == std::string_view::npos ? path.size() - start
                                                      : end - start));
            if (!current->is_object()) return nullptr;
            const auto found = current->find(key);
            if (found == current->end()) return nullptr;
            current = &*found;
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return current;
    };
    const auto publicId = [](const std::string_view value) {
        if (value.empty() || value.size() > 128 ||
            !std::isalpha(static_cast<unsigned char>(value.front())))
            return false;
        return std::ranges::all_of(value, [](const unsigned char character) {
            return std::isalnum(character) || character == '_' ||
                   character == '-' || character == '.';
        });
    };

    std::unordered_set<std::string> propertyIds;
    for (const auto& item : *properties) {
        UiComponentProperty property;
        std::string valueType;
        const bool fields = item.is_object() &&
                            ReadString(item, "id", property.id) &&
                            publicId(property.id) &&
                            propertyIds.insert(property.id).second &&
                            ReadString(item, "displayName",
                                       property.displayName) &&
                            ReadString(item, "nodeId", property.nodeId) &&
                            IsUuid(property.nodeId) &&
                            ReadString(item, "property", property.property) &&
                            ReadString(item, "valueType", valueType) &&
                            metadataValid(item);
        const auto type = fields ? parseValueType(valueType) : std::nullopt;
        const auto defaultValue = item.find("defaultValue");
        const auto source = sourceNodes.find(property.nodeId);
        const Json* authored =
            source == sourceNodes.end()
                ? nullptr
                : propertyAtPath(*source->second, property.property);
        if (!fields || !type || defaultValue == item.end() ||
            !valueMatches(*type, *defaultValue) || !authored ||
            !valueMatches(*type, *authored)) {
            AddDiagnostic(result, "PXSDKUICOMP1105",
                          "UI component exposed property is invalid" +
                              (property.id.empty()
                                   ? std::string{}
                                   : ": " + property.id));
            continue;
        }
        property.valueType = *type;
        property.defaultValue = {defaultValue->dump()};
        result.document.componentInterface.properties.push_back(
            std::move(property));
    }

    std::unordered_set<std::string> signalIds;
    for (const auto& item : *signals) {
        UiComponentSignal signal;
        const bool fields = item.is_object() &&
                            ReadString(item, "id", signal.id) &&
                            publicId(signal.id) &&
                            signalIds.insert(signal.id).second &&
                            ReadString(item, "displayName",
                                       signal.displayName) &&
                            ReadString(item, "nodeId", signal.nodeId) &&
                            IsUuid(signal.nodeId) &&
                            ReadString(item, "signal", signal.signal) &&
                            metadataValid(item);
        const auto source = sourceNodes.find(signal.nodeId);
        const auto arguments = item.find("arguments");
        bool valid = fields && source != sourceNodes.end() &&
                     arguments != item.end() && arguments->is_array();
        if (valid) {
            const auto kind = source->second->find("kind");
            const auto sourceKind =
                kind != source->second->end() && kind->is_string()
                    ? ParseKind(kind->get_ref<const std::string&>())
                    : std::nullopt;
            const auto runtimeType = source->second->find("runtimeType");
            const bool dynamicSignal =
                result.document.content.schemaRevision == 2 &&
                runtimeType != source->second->end() &&
                runtimeType->is_string() && !signal.signal.empty() &&
                signal.signal.size() <= 128;
            valid = sourceKind &&
                    (ValidSignal(*sourceKind, signal.signal) || dynamicSignal);
        }
        std::unordered_set<std::string> argumentIds;
        if (arguments != item.end() && arguments->is_array()) {
            for (const auto& argument : *arguments) {
                UiComponentSignalArgument parsedArgument;
                std::string typeName;
                const bool argumentFields =
                    argument.is_object() &&
                    ReadString(argument, "id", parsedArgument.id) &&
                    publicId(parsedArgument.id) &&
                    argumentIds.insert(parsedArgument.id).second &&
                    ReadString(argument, "valueType", typeName);
                const auto type = argumentFields
                                      ? parseValueType(typeName)
                                      : std::nullopt;
                if (!argumentFields || !type) {
                    valid = false;
                    continue;
                }
                parsedArgument.valueType = *type;
                signal.arguments.push_back(std::move(parsedArgument));
            }
        }
        if (!valid) {
            AddDiagnostic(result, "PXSDKUICOMP1106",
                          "UI component exposed signal is invalid");
            continue;
        }
        result.document.componentInterface.signals.push_back(std::move(signal));
    }

    std::unordered_set<std::string> slotIds;
    for (const auto& item : *slots) {
        UiComponentSlotDefinition slot;
        const bool fields = item.is_object() &&
                            ReadString(item, "id", slot.id) &&
                            publicId(slot.id) &&
                            slotIds.insert(slot.id).second &&
                            ReadString(item, "displayName", slot.displayName) &&
                            ReadString(item, "nodeId", slot.nodeId) &&
                            IsUuid(slot.nodeId) && metadataValid(item);
        const auto source = sourceNodes.find(slot.nodeId);
        bool valid = fields && source != sourceNodes.end();
        if (valid) {
            const auto kind = source->second->find("kind");
            valid = kind != source->second->end() && kind->is_string() &&
                    kind->get<std::string>() != "button" &&
                    kind->get<std::string>() != "label" &&
                    kind->get<std::string>() != "image";
        }
        if (!valid) {
            AddDiagnostic(result, "PXSDKUICOMP1107",
                          "UI component slot is invalid");
            continue;
        }
        result.document.componentInterface.slots.push_back(std::move(slot));
    }
    return result;
}

}  // namespace px::sdk
