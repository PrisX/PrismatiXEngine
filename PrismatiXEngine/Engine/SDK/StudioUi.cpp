#include "Engine/SDK/StudioUi.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace px::sdk {
namespace {

using Json = nlohmann::json;

void AddDiagnostic(StudioUiParseResult& result, std::string code,
                   std::string message, const std::size_t nodeIndex = 0) {
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

std::optional<StudioUiNodeKind> ParseKind(const std::string_view value) {
    if (value == "control") return StudioUiNodeKind::Control;
    if (value == "label") return StudioUiNodeKind::Label;
    if (value == "button") return StudioUiNodeKind::Button;
    if (value == "image") return StudioUiNodeKind::Image;
    if (value == "stack") return StudioUiNodeKind::Stack;
    if (value == "hbox") return StudioUiNodeKind::HBox;
    if (value == "vbox") return StudioUiNodeKind::VBox;
    if (value == "grid") return StudioUiNodeKind::Grid;
    if (value == "group") return StudioUiNodeKind::Group;
    return std::nullopt;
}

bool OwnsLayout(const StudioUiNodeKind kind) {
    return kind == StudioUiNodeKind::Stack || kind == StudioUiNodeKind::HBox ||
           kind == StudioUiNodeKind::VBox || kind == StudioUiNodeKind::Grid;
}

bool ReadFiniteFloat(const Json& object, const char* key, float& value) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return false;
    value = found->get<float>();
    return std::isfinite(value);
}

bool ParseValue(const Json& value, StudioUiValue& output) {
    if (value.is_null()) output = std::monostate{};
    else if (value.is_boolean()) output = value.get<bool>();
    else if (value.is_number_integer()) output = value.get<std::int64_t>();
    else if (value.is_number()) output = value.get<double>();
    else if (value.is_string()) output = value.get<std::string>();
    else if (value.is_object()) {
        std::string type;
        if (!ReadString(value, "type", type)) return false;
        if (type == "vec2") {
            StudioUiVec2Value parsed;
            if (value.size() != 3 || !ReadFiniteFloat(value, "x", parsed.x) ||
                !ReadFiniteFloat(value, "y", parsed.y)) return false;
            output = parsed;
        } else if (type == "rect") {
            StudioUiRectValue parsed;
            if (value.size() != 5 || !ReadFiniteFloat(value, "x", parsed.x) ||
                !ReadFiniteFloat(value, "y", parsed.y) ||
                !ReadFiniteFloat(value, "width", parsed.width) ||
                !ReadFiniteFloat(value, "height", parsed.height)) return false;
            output = parsed;
        } else if (type == "color") {
            StudioUiColorValue parsed;
            if (value.size() != 2 || !ReadString(value, "value", parsed.value) ||
                !IsColor(parsed.value)) return false;
            output = std::move(parsed);
        } else if (type == "nodeReference") {
            StudioUiNodeReferenceValue parsed;
            if (value.size() != 2 || !ReadString(value, "nodeId", parsed.nodeId) ||
                !IsUuid(parsed.nodeId)) return false;
            output = std::move(parsed);
        } else return false;
    } else return false;
    return true;
}

bool ParseActionValue(const Json& value, StudioUiActionValue& output) {
    if (!ParseValue(value, output)) return false;
    return output.index() <= 4;
}

bool ReadValueMap(const Json& value,
                  std::unordered_map<std::string, StudioUiValue>& output) {
    if (!value.is_object()) return false;
    for (auto item = value.begin(); item != value.end(); ++item) {
        StudioUiValue parsed;
        if (item.key().empty() || !ParseValue(item.value(), parsed)) return false;
        output.emplace(item.key(), std::move(parsed));
    }
    return true;
}

std::optional<StudioUiBehaviorNodeKind> ParseBehaviorKind(
    const std::string_view value) {
    if (value == "signalEntry") return StudioUiBehaviorNodeKind::SignalEntry;
    if (value == "action") return StudioUiBehaviorNodeKind::Action;
    if (value == "sequence") return StudioUiBehaviorNodeKind::Sequence;
    if (value == "branch") return StudioUiBehaviorNodeKind::Branch;
    if (value == "delay") return StudioUiBehaviorNodeKind::Delay;
    if (value == "constant") return StudioUiBehaviorNodeKind::Constant;
    if (value == "compare") return StudioUiBehaviorNodeKind::Compare;
    if (value == "boolean") return StudioUiBehaviorNodeKind::Boolean;
    if (value == "getVariable") return StudioUiBehaviorNodeKind::GetVariable;
    if (value == "setVariable") return StudioUiBehaviorNodeKind::SetVariable;
    if (value == "getProperty") return StudioUiBehaviorNodeKind::GetProperty;
    if (value == "setProperty") return StudioUiBehaviorNodeKind::SetProperty;
    if (value == "playAnimation") return StudioUiBehaviorNodeKind::PlayAnimation;
    if (value == "setAnimationParameter")
        return StudioUiBehaviorNodeKind::SetAnimationParameter;
    if (value == "travelAnimationState")
        return StudioUiBehaviorNodeKind::TravelAnimationState;
    return std::nullopt;
}

std::optional<StudioUiBehaviorReentry> ParseBehaviorReentry(
    const std::string_view value) {
    if (value == "allow") return StudioUiBehaviorReentry::Allow;
    if (value == "ignoreWhileRunning")
        return StudioUiBehaviorReentry::IgnoreWhileRunning;
    if (value == "restart") return StudioUiBehaviorReentry::Restart;
    return std::nullopt;
}

std::optional<StudioUiAnimationEase> ParseAnimationEase(
    const std::string_view value) {
    if (value == "linear") return StudioUiAnimationEase::Linear;
    if (value == "easeIn") return StudioUiAnimationEase::EaseIn;
    if (value == "easeOut") return StudioUiAnimationEase::EaseOut;
    if (value == "easeInOut") return StudioUiAnimationEase::EaseInOut;
    if (value == "step") return StudioUiAnimationEase::Step;
    return std::nullopt;
}

std::optional<StudioUiAnimationInterpolation> ParseAnimationInterpolation(
    const std::string_view value) {
    if (value == "linear") return StudioUiAnimationInterpolation::Linear;
    if (value == "discrete") return StudioUiAnimationInterpolation::Discrete;
    return std::nullopt;
}

std::optional<StudioUiAnimationParameterType> ParseAnimationParameterType(
    const std::string_view value) {
    if (value == "trigger") return StudioUiAnimationParameterType::Trigger;
    if (value == "bool") return StudioUiAnimationParameterType::Bool;
    if (value == "number") return StudioUiAnimationParameterType::Number;
    return std::nullopt;
}

std::optional<StudioUiAnimationConditionOperator> ParseAnimationCondition(
    const std::string_view value) {
    if (value == "triggered")
        return StudioUiAnimationConditionOperator::Triggered;
    if (value == "equal") return StudioUiAnimationConditionOperator::Equal;
    if (value == "notEqual") return StudioUiAnimationConditionOperator::NotEqual;
    if (value == "less") return StudioUiAnimationConditionOperator::Less;
    if (value == "lessEqual")
        return StudioUiAnimationConditionOperator::LessEqual;
    if (value == "greater") return StudioUiAnimationConditionOperator::Greater;
    if (value == "greaterEqual")
        return StudioUiAnimationConditionOperator::GreaterEqual;
    return std::nullopt;
}

const StudioUiValue* Property(const StudioUiBehaviorNode& node,
                              const std::string_view name) {
    const auto found = node.properties.find(std::string(name));
    return found == node.properties.end() ? nullptr : &found->second;
}

bool IsNumber(const StudioUiValue& value) {
    return std::holds_alternative<std::int64_t>(value) ||
           std::holds_alternative<double>(value);
}

std::optional<double> Number(const StudioUiValue* value) {
    if (!value) return std::nullopt;
    if (const auto* integer = std::get_if<std::int64_t>(value))
        return static_cast<double>(*integer);
    if (const auto* number = std::get_if<double>(value)) return *number;
    return std::nullopt;
}

const std::string* String(const StudioUiValue* value) {
    return value ? std::get_if<std::string>(value) : nullptr;
}

bool KnownAction(const std::string_view id) {
    static const std::unordered_set<std::string_view> actions = {
        "game.start", "app.quit", "overlay.close", "load.open", "save.open",
        "gallery.open", "settings.open", "backlog.open", "choice.select",
        "load.slot", "save.slot", "cg.view", "mode.auto", "mode.skip",
        "set.skipread.toggle", "set.fullscreen.toggle", "animation.trigger",
        "animation.bool", "animation.number", "animation.travel"};
    return actions.contains(id);
}

bool ValidAction(const StudioUiAction& action) {
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
    return false;
}

bool SupportedRuntimeProperty(const std::string_view property) {
    static const std::unordered_set<std::string_view> names = {
        "opacity", "rotation", "scale", "pivot", "modulate", "visibility",
        "enabled", "offsets", "minimumSize"};
    return names.contains(property);
}

bool RuntimePropertyValueMatches(const std::string_view property,
                                 const StudioUiValue& value) {
    if (property == "opacity" || property == "rotation") return IsNumber(value);
    if (property == "scale" || property == "pivot" ||
        property == "minimumSize")
        return std::holds_alternative<StudioUiVec2Value>(value);
    if (property == "modulate")
        return std::holds_alternative<StudioUiColorValue>(value);
    if (property == "visibility")
        return std::get_if<std::string>(&value) &&
               (*std::get_if<std::string>(&value) == "Visible" ||
                *std::get_if<std::string>(&value) == "Hidden" ||
                *std::get_if<std::string>(&value) == "Collapsed");
    if (property == "enabled") return std::holds_alternative<bool>(value);
    if (property == "offsets")
        return std::holds_alternative<StudioUiRectValue>(value);
    return false;
}

bool ValidSignal(const StudioUiNodeKind kind, const std::string_view signal) {
    static const std::unordered_set<std::string_view> controlSignals = {
        "pointerEntered", "pointerExited", "pointerDown", "pointerUp", "clicked",
        "scrolled", "focusEntered", "focusExited"};
    return controlSignals.contains(signal) ||
           (kind == StudioUiNodeKind::Button && signal == "activated");
}

bool AllowedProperties(
    const StudioUiBehaviorNode& node,
    const std::initializer_list<std::string_view> allowed) {
    return std::all_of(node.properties.begin(), node.properties.end(),
                       [&](const auto& item) {
                           return std::find(allowed.begin(), allowed.end(),
                                            item.first) != allowed.end();
                       });
}

bool ValidateBehaviorNode(const StudioUiBehaviorNode& node,
                          const std::unordered_set<std::string>& uiNodeIds) {
    using Kind = StudioUiBehaviorNodeKind;
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
                ? std::get_if<StudioUiNodeReferenceValue>(
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

bool SameAnimationValueType(const StudioUiValue& left,
                            const StudioUiValue& right) {
    if (IsNumber(left) && IsNumber(right)) return true;
    return left.index() == right.index();
}

bool BehaviorOutputPin(const StudioUiBehaviorNode& node,
                       const std::string_view pin) {
    using Kind = StudioUiBehaviorNodeKind;
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

bool BehaviorInputPin(const StudioUiBehaviorNode& node,
                      const std::string_view pin) {
    using Kind = StudioUiBehaviorNodeKind;
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

bool FlowOutputPin(const StudioUiBehaviorNode& node,
                   const std::string_view pin) {
    using Kind = StudioUiBehaviorNodeKind;
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
    const Json& root, StudioUiParseResult& result,
    std::unordered_set<std::string>& identities,
    const std::unordered_map<std::string, const StudioUiNode*>& uiNodes) {
    const auto graphValue = root.find("behaviorGraph");
    if (graphValue != root.end()) {
        if (!graphValue->is_object()) {
            AddDiagnostic(result, "PXSDKUI1050",
                          "Studio UI behaviorGraph must be an object");
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
                    StudioUiBehaviorNode node;
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
                    StudioUiBehaviorLink link;
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
                    StudioUiBehaviorGroup group;
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

    std::unordered_map<std::string, const StudioUiBehaviorNode*> graphNodes;
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
                      "Studio UI behaviorTriggers must be an array");
        return;
    }
    std::unordered_set<std::string> triggerSources;
    for (const auto& item : *triggers) {
        StudioUiBehaviorTrigger trigger;
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
        if (source == uiNodes.end() ||
            !ValidSignal(source->second->kind, trigger.signal) ||
            entry == graphNodes.end() ||
            entry->second->kind != StudioUiBehaviorNodeKind::SignalEntry ||
            !triggerSources.insert(sourceSignal).second) {
            AddDiagnostic(
                result, "PXSDKUI1062",
                "Behavior trigger must reference one valid signal and Signal Entry");
            continue;
        }
        result.document.behaviorTriggers.push_back(std::move(trigger));
    }
}

void ParseAnimationSection(
    const Json& root, StudioUiParseResult& result,
    std::unordered_set<std::string>& identities,
    const std::unordered_map<std::string, const StudioUiNode*>& uiNodes) {
    const auto value = root.find("animations");
    if (value == root.end() || value->is_null()) return;
    if (!value->is_object()) {
        AddDiagnostic(result, "PXSDKUI1070",
                      "Studio UI animations must be an object or null");
        return;
    }
    StudioUiAnimations animations;
    const auto clips = value->find("clips");
    const auto machine = value->find("stateMachine");
    if (clips == value->end() || !clips->is_array() ||
        machine == value->end() || !machine->is_object()) {
        AddDiagnostic(
            result, "PXSDKUI1071",
            "Animations require clips and a named stateMachine object");
        return;
    }

    std::unordered_map<std::string, const StudioUiNode*> nodes = uiNodes;
    std::unordered_set<std::string> clipIds;
    std::unordered_set<std::string> clipNames;
    for (const auto& item : *clips) {
        StudioUiAnimationClip clip;
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
            StudioUiAnimationTrack track;
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
                StudioUiAnimationKey key;
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

    std::unordered_map<std::string, StudioUiAnimationParameterType>
        parameterTypes;
    for (const auto& item : *parameters) {
        StudioUiAnimationParameter parameter;
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
            ((*parsedType == StudioUiAnimationParameterType::Trigger ||
              *parsedType == StudioUiAnimationParameterType::Bool) &&
             !std::holds_alternative<bool>(parameter.defaultValue)) ||
            (*parsedType == StudioUiAnimationParameterType::Number &&
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
        StudioUiAnimationState state;
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
        StudioUiAnimationTransition transition;
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
            StudioUiAnimationCondition condition;
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
                (type == StudioUiAnimationParameterType::Trigger &&
                 *parsedOperation ==
                     StudioUiAnimationConditionOperator::Triggered &&
                 std::holds_alternative<bool>(condition.value)) ||
                (type == StudioUiAnimationParameterType::Bool &&
                 (*parsedOperation ==
                      StudioUiAnimationConditionOperator::Equal ||
                  *parsedOperation ==
                      StudioUiAnimationConditionOperator::NotEqual) &&
                 std::holds_alternative<bool>(condition.value)) ||
                (type == StudioUiAnimationParameterType::Number &&
                 *parsedOperation !=
                     StudioUiAnimationConditionOperator::Triggered &&
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

void ValidateBehaviorAnimationReferences(StudioUiParseResult& result) {
    using Kind = StudioUiBehaviorNodeKind;
    std::unordered_set<std::string> stateNames;
    std::unordered_map<std::string, StudioUiAnimationParameterType> parameters;
    if (result.document.animations) {
        for (const auto& state :
             result.document.animations->stateMachine.states)
            stateNames.insert(state.name);
        for (const auto& parameter :
             result.document.animations->stateMachine.parameters)
            parameters.emplace(parameter.name, parameter.type);
    }
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
                ((found->second == StudioUiAnimationParameterType::Trigger) ||
                 (found->second == StudioUiAnimationParameterType::Bool &&
                  std::holds_alternative<bool>(*value)) ||
                 (found->second == StudioUiAnimationParameterType::Number &&
                  IsNumber(*value)));
            if (!matches)
                AddDiagnostic(
                    result, "PXSDKUI1086",
                    "Behavior animation parameter must exist with a matching type");
        }
    }
}

}  // namespace

StudioUiParseResult ParseStudioUi(const std::string_view text) {
    StudioUiParseResult result;
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        AddDiagnostic(result, "PXSDKUI1001", "Studio UI must be a JSON object");
        return result;
    }
    if (root.value("format", std::string{}) != "PrismatiXUIScene")
        AddDiagnostic(result, "PXSDKUI1002", "Studio UI format is not PrismatiXUIScene");
    const auto schemaRevision = root.find("schemaRevision");
    if (schemaRevision == root.end() || !schemaRevision->is_number_unsigned() ||
        schemaRevision->get<std::uint32_t>() != 1)
        AddDiagnostic(result, "PXSDKUI1003", "Unsupported Studio UI schema revision");
    if (!ReadString(root, "id", result.document.id) || !IsUuid(result.document.id))
        AddDiagnostic(result, "PXSDKUI1004", "Studio UI id must be a UUID");
    const auto revision = root.find("revision");
    if (revision == root.end() || !revision->is_number_unsigned())
        AddDiagnostic(result, "PXSDKUI1005", "Studio UI revision is required");
    else result.document.revision = revision->get<std::uint64_t>();
    if (!ReadString(root, "name", result.document.name))
        AddDiagnostic(result, "PXSDKUI1006", "Studio UI name is required");
    const auto width = root.find("width"), height = root.find("height");
    if (width == root.end() || !width->is_number_unsigned() || width->get<std::uint32_t>() == 0 ||
        height == root.end() || !height->is_number_unsigned() || height->get<std::uint32_t>() == 0) {
        AddDiagnostic(result, "PXSDKUI1007", "Studio UI artboard size must be positive");
    } else {
        result.document.width = width->get<std::uint32_t>();
        result.document.height = height->get<std::uint32_t>();
    }
    if (!ReadString(root, "rootId", result.document.rootId) || !IsUuid(result.document.rootId))
        AddDiagnostic(result, "PXSDKUI1008", "Studio UI rootId must be a UUID");

    const auto nodes = root.find("nodes");
    if (nodes == root.end() || !nodes->is_array()) {
        AddDiagnostic(result, "PXSDKUI1009", "Studio UI nodes must be an array");
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
        StudioUiNode node;
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
            else node.layout.mode = mode == "free" ? StudioUiLayoutMode::Free : StudioUiLayoutMode::Container;
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
            if (!layoutValid || node.layout.width <= 0.0f || node.layout.height <= 0.0f ||
                node.layout.anchorX < 0.0f || node.layout.anchorX > 1.0f ||
                node.layout.anchorY < 0.0f || node.layout.anchorY > 1.0f ||
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
                if (!IsUuid(*node.assetId) || node.kind != StudioUiNodeKind::Image) {
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
                StudioUiAction action;
                bool actionValid = node.kind == StudioUiNodeKind::Button &&
                                   ReadString(*onClick, "id", action.id);
                const auto arguments = onClick->find("arguments");
                if (arguments == onClick->end() || !arguments->is_object()) actionValid = false;
                else for (auto argument = arguments->begin(); argument != arguments->end(); ++argument) {
                    StudioUiActionValue value;
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
        const auto componentInstance = item.find("componentInstance");
        if (componentInstance != item.end() && !componentInstance->is_null()) {
            StudioUiComponentInstance instance;
            static const std::unordered_set<std::string> overridePaths{
                "name", "visible", "locked", "layout.x", "layout.y",
                "layout.width", "layout.height", "layout.anchorX",
                "layout.anchorY", "layout.pivotX", "layout.pivotY",
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
                    if (!path.is_string() ||
                        !overridePaths.contains(path.get<std::string>()) ||
                        !uniqueOverrides.insert(path.get<std::string>()).second) {
                        instanceValid = false;
                        continue;
                    }
                    instance.overrides.push_back(path.get<std::string>());
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
        if (valid) result.document.nodes.push_back(std::move(node));
    }

    const auto theme = root.find("theme");
    if (theme == root.end() || !theme->is_array()) {
        AddDiagnostic(result, "PXSDKUI1030", "Studio UI theme must be an array");
    } else {
        std::unordered_set<std::string> names;
        for (const auto& item : *theme) {
            StudioUiThemeToken token;
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

    std::unordered_map<std::string, const StudioUiNode*> byId;
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
        const StudioUiNode* cursor = &node;
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
    }
    ParseBehaviorSections(root, result, identities, byId);
    ParseAnimationSection(root, result, identities, byId);
    ValidateBehaviorAnimationReferences(result);
    const auto rootNode = byId.find(result.document.rootId);
    if (rootNode == byId.end() || rootNode->second->parentId ||
        rootNode->second->kind != StudioUiNodeKind::Control) {
        AddDiagnostic(result, "PXSDKUI1040", "Studio UI root must be a parentless control");
    }
    if (std::count_if(result.document.nodes.begin(), result.document.nodes.end(),
                      [](const StudioUiNode& node) { return !node.parentId; }) != 1)
        AddDiagnostic(result, "PXSDKUI1041", "Studio UI requires exactly one root");
    for (const auto& node : result.document.nodes) {
        if (!node.parentId) continue;
        const auto parent = byId.find(*node.parentId);
        if (parent == byId.end()) {
            AddDiagnostic(result, "PXSDKUI1042", "UI node references a missing parent");
            continue;
        }
        if (OwnsLayout(parent->second->kind) && node.layout.mode != StudioUiLayoutMode::Container)
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

}  // namespace px::sdk
