#include "Engine/Session/RuntimeIrAdapter.h"

#include "Engine/Core/NumberParsing.h"

#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Expression/Expression.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace px {
namespace {

using Operation = sdk::RuntimeIrOperation;

const std::string& Argument(const Operation& operation, const std::string& name) {
    static const std::string empty;
    const auto found = operation.arguments.find(name);
    return found == operation.arguments.end() ? empty : found->second;
}

void Add(vn::Command& command, std::string key, const std::string& value) {
    if (!value.empty()) command.args.push_back({std::move(key), value});
}

std::optional<Variant> UntypedJsonValue(const nlohmann::json& value, int depth = 0) {
    if (depth > 32) return std::nullopt;
    if (value.is_null()) return Variant{};
    if (value.is_boolean()) return Variant(value.get<bool>());
    if (value.is_number_integer()) return Variant(value.get<std::int64_t>());
    if (value.is_number()) return Variant(value.get<double>());
    if (value.is_string()) return Variant(value.get<std::string>());
    if (value.is_array()) {
        VariantArray result;
        for (const auto& item : value) {
            auto converted = UntypedJsonValue(item, depth + 1);
            if (!converted) return std::nullopt;
            result.push_back(std::move(*converted));
        }
        return Variant(std::move(result));
    }
    if (value.is_object()) {
        VariantObject result;
        for (auto item = value.begin(); item != value.end(); ++item) {
            auto converted = UntypedJsonValue(item.value(), depth + 1);
            if (!converted) return std::nullopt;
            result.emplace(item.key(), std::move(*converted));
        }
        return Variant(std::move(result));
    }
    return std::nullopt;
}

std::optional<Variant> TypedJsonValue(const nlohmann::json& value,
                                      const VariantType type) {
    if (type == VariantType::Null && value.is_null()) return Variant{};
    if (type == VariantType::Bool && value.is_boolean()) return Variant(value.get<bool>());
    if (type == VariantType::Integer && value.is_number_integer()) {
        return Variant(value.get<std::int64_t>());
    }
    if (type == VariantType::Number && value.is_number()) return Variant(value.get<double>());
    if (type == VariantType::String && value.is_string()) {
        return Variant(value.get<std::string>());
    }
    if (type == VariantType::Uuid && value.is_string()) {
        if (const auto parsed = Uuid::Parse(value.get<std::string>())) return Variant(*parsed);
        return std::nullopt;
    }
    if (type == VariantType::TokenRef && value.is_string()) {
        return Variant(TokenRefValue{value.get<std::string>()});
    }
    if (type == VariantType::Vec2) {
        if (value.is_array() && value.size() == 2 && value[0].is_number() &&
            value[1].is_number()) {
            return Variant(Vec2{value[0].get<float>(), value[1].get<float>()});
        }
        if (value.is_object() && value.contains("x") && value["x"].is_number() &&
            value.contains("y") && value["y"].is_number()) {
            return Variant(Vec2{value["x"].get<float>(), value["y"].get<float>()});
        }
        return std::nullopt;
    }
    if (type == VariantType::Rect && value.is_array() && value.size() == 4 &&
        std::ranges::all_of(value, [](const auto& item) { return item.is_number(); })) {
        return Variant(Rect{value[0].get<float>(), value[1].get<float>(),
                            value[2].get<float>(), value[3].get<float>()});
    }
    if (type == VariantType::Color && value.is_array() && value.size() == 4) {
        int channels[4]{};
        for (int index = 0; index < 4; ++index) {
            if (!value[index].is_number_integer()) return std::nullopt;
            channels[index] = value[index].get<int>();
            if (channels[index] < 0 || channels[index] > 255) return std::nullopt;
        }
        return Variant(Color{static_cast<std::uint8_t>(channels[0]),
                             static_cast<std::uint8_t>(channels[1]),
                             static_cast<std::uint8_t>(channels[2]),
                             static_cast<std::uint8_t>(channels[3])});
    }
    if (type == VariantType::ResourceRef) {
        if (value.is_string()) {
            const std::string path = value.get<std::string>();
            if (path.starts_with("asset:")) {
                if (const auto parsed = Uuid::Parse(path.substr(6))) {
                    return Variant(ResourceRefValue{*parsed, path});
                }
                return std::nullopt;
            }
            return Variant(ResourceRefValue{Uuid::FromName(path), path});
        }
        if (value.is_object()) {
            std::string idText;
            if (const auto id = value.find("id"); id != value.end() && id->is_string()) {
                idText = id->get<std::string>();
            } else if (const auto uuid = value.find("uuid");
                       uuid != value.end() && uuid->is_string()) {
                idText = uuid->get<std::string>();
            }
            std::string path;
            if (const auto found = value.find("path");
                found != value.end() && found->is_string()) {
                path = found->get<std::string>();
            }
            Uuid id;
            if (!idText.empty()) {
                const auto parsed = Uuid::Parse(idText);
                if (!parsed) return std::nullopt;
                id = *parsed;
            } else if (!path.empty()) {
                id = Uuid::FromName(path);
            } else {
                return std::nullopt;
            }
            return Variant(ResourceRefValue{id, std::move(path)});
        }
        return std::nullopt;
    }
    if (type == VariantType::Array && value.is_array()) return UntypedJsonValue(value);
    if (type == VariantType::Object && value.is_object()) return UntypedJsonValue(value);
    return std::nullopt;
}

void AddCommandValue(vn::Command& command, const std::string& name, Variant value) {
    if (const auto* text = value.TryGet<std::string>()) {
        command.args.push_back({name, *text});
    } else {
        command.typedArgs[name] = std::move(value);
    }
}

bool MapTypedCustomNode(vn::Command& command,
                        const vn::CommandDescriptor& descriptor,
                        const std::string& payload,
                        const std::size_t operationIndex,
                        std::vector<std::string>& errors) {
    const nlohmann::json arguments = nlohmann::json::parse(payload, nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) {
        errors.push_back("custom command '" + descriptor.id +
                         "' requires a JSON argument object at Runtime IR operation " +
                         std::to_string(operationIndex));
        return false;
    }
    std::unordered_map<std::string, const vn::CommandParameterDescriptor*> parameters;
    for (const auto& parameter : descriptor.parameters) {
        parameters.emplace(parameter.name, &parameter);
        const auto found = arguments.find(parameter.name);
        if (found == arguments.end()) {
            if (parameter.hasDefault) {
                AddCommandValue(command, parameter.name, parameter.defaultValue.Clone());
            }
            continue;
        }
        auto converted = TypedJsonValue(*found, parameter.type);
        if (!converted) {
            errors.push_back("custom command '" + descriptor.id + "' parameter '" +
                             parameter.name + "' has the wrong JSON type at Runtime IR operation " +
                             std::to_string(operationIndex));
            continue;
        }
        AddCommandValue(command, parameter.name, std::move(*converted));
    }
    for (auto field = arguments.begin(); field != arguments.end(); ++field) {
        if (parameters.contains(field.key())) continue;
        auto converted = UntypedJsonValue(field.value());
        if (!converted) {
            errors.push_back("custom command '" + descriptor.id + "' parameter '" +
                             field.key() + "' could not be converted at Runtime IR operation " +
                             std::to_string(operationIndex));
            continue;
        }
        AddCommandValue(command, field.key(), std::move(*converted));
    }
    return true;
}

std::string LabelTarget(const std::string& name) {
    if (name.empty() || name.front() == '@') return name;
    return '@' + name;
}

std::string Trim(std::string_view value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(start, end - start + 1));
}

Variant ParseLiteral(const std::string& text) {
    const std::string value = Trim(text);
    if (value == "true") return Variant(true);
    if (value == "false") return Variant(false);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return Variant(value.substr(1, value.size() - 2));
    }
    std::int64_t integer = 0;
    const auto integerResult =
        std::from_chars(value.data(), value.data() + value.size(), integer);
    if (integerResult.ec == std::errc{} && integerResult.ptr == value.data() + value.size()) {
        return Variant(integer);
    }
    double number = 0.0;
    if (ParseFiniteDouble(value, number)) {
        return Variant(number);
    }
    return Variant(value);
}

std::optional<Variant> ParseCatalogLiteral(const std::string& text,
                                           const std::string_view type) {
    const std::string value = Trim(text);
    if (type == "boolean") {
        if (value == "true") return Variant(true);
        if (value == "false") return Variant(false);
        return std::nullopt;
    }
    if (type == "integer") {
        std::int64_t integer = 0;
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), integer);
        return parsed.ec == std::errc{} &&
                       parsed.ptr == value.data() + value.size()
                   ? std::optional<Variant>(Variant(integer))
                   : std::nullopt;
    }
    if (type == "number") {
        double number = 0.0;
        return ParseFiniteDouble(value, number)
                   ? std::optional<Variant>(Variant(number))
                   : std::nullopt;
    }
    if (type == "string") {
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            const auto decoded = nlohmann::json::parse(value, nullptr, false);
            if (decoded.is_discarded() || !decoded.is_string()) return std::nullopt;
            return Variant(decoded.get<std::string>());
        }
        if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
            return Variant(value.substr(1, value.size() - 2));
        }
        return Variant(value);
    }
    return std::nullopt;
}

std::optional<vn::Expression> ParseSimpleExpression(const std::string& text) {
    struct Candidate {
        std::string_view token;
        vn::ExpressionOperator operation;
    };
    constexpr Candidate candidates[] = {
        {"==", vn::ExpressionOperator::Equal},
        {"!=", vn::ExpressionOperator::NotEqual},
        {">=", vn::ExpressionOperator::GreaterEqual},
        {"<=", vn::ExpressionOperator::LessEqual},
        {">", vn::ExpressionOperator::Greater},
        {"<", vn::ExpressionOperator::Less},
    };
    for (const auto& candidate : candidates) {
        const auto offset = text.find(candidate.token);
        if (offset == std::string::npos) continue;
        std::string variable = Trim(std::string_view(text).substr(0, offset));
        if (variable.starts_with('$')) variable.erase(0, 1);
        const std::string literal = Trim(
            std::string_view(text).substr(offset + candidate.token.size()));
        if (variable.empty() || literal.empty()) return std::nullopt;
        return vn::Expression::Binary(candidate.operation,
                                      vn::Expression::Variable(std::move(variable)),
                                      vn::Expression::Literal(ParseLiteral(literal)));
    }
    std::string variable = Trim(text);
    if (variable.starts_with('$')) variable.erase(0, 1);
    if (variable.empty()) return std::nullopt;
    return vn::Expression::Variable(std::move(variable));
}

std::optional<vn::Expression> ParseRuntimeExpression(const std::string& text) {
    const std::string trimmed = Trim(text);
    if (trimmed.empty()) return std::nullopt;
    if (trimmed.front() != '{') return ParseSimpleExpression(trimmed);
    const nlohmann::json encoded = nlohmann::json::parse(text, nullptr, false);
    if (encoded.is_discarded() || !encoded.is_object()) return std::nullopt;
    auto value = UntypedJsonValue(encoded);
    if (!value) return std::nullopt;
    auto expression = vn::ExpressionFromValue(*value);
    return expression ? std::optional<vn::Expression>(expression.TakeValue())
                      : std::nullopt;
}

std::string DurationMilliseconds(const std::string& value) {
    std::string_view number = value;
    double scale = 1.0;
    if (number.ends_with("ms")) {
        number.remove_suffix(2);
    } else if (number.ends_with('s')) {
        number.remove_suffix(1);
        scale = 1000.0;
    }
    double parsed = 0.0;
    if (!ParseFiniteDouble(number, parsed) || parsed < 0.0) {
        return {};
    }
    return std::to_string(static_cast<std::uint64_t>(parsed * scale));
}

struct ConditionBoundaries {
    std::size_t elseIndex = (std::numeric_limits<std::size_t>::max)();
    std::size_t endIndex = (std::numeric_limits<std::size_t>::max)();
};

std::unordered_map<std::size_t, ConditionBoundaries> FindConditionBoundaries(
    const std::vector<Operation>& operations,
    std::unordered_map<std::size_t, std::size_t>& ownerByBoundary) {
    std::unordered_map<std::size_t, ConditionBoundaries> boundaries;
    std::vector<std::size_t> stack;
    for (std::size_t index = 0; index < operations.size(); ++index) {
        if (operations[index].kind == "condition") {
            stack.push_back(index);
        } else if (operations[index].kind == "else" && !stack.empty()) {
            boundaries[stack.back()].elseIndex = index;
            ownerByBoundary[index] = stack.back();
        } else if (operations[index].kind == "endCondition" && !stack.empty()) {
            boundaries[stack.back()].endIndex = index;
            ownerByBoundary[index] = stack.back();
            stack.pop_back();
        }
    }
    return boundaries;
}

vn::Command Label(std::string name, const int line, std::string sourceId = {},
                  std::string operationId = {}) {
    vn::Command command;
    command.type = "label";
    command.line = line;
    command.sourceId = std::move(sourceId);
    command.operationId = std::move(operationId);
    command.args.push_back({"name", LabelTarget(name)});
    return command;
}

}  // namespace

vn::Program CompileRuntimeIr(const sdk::RuntimeIrDocument& document) {
    std::vector<vn::Command> commands;
    std::unordered_map<std::size_t, std::size_t> ownerByBoundary;
    const auto conditions = FindConditionBoundaries(document.operations, ownerByBoundary);
    std::vector<std::string> adapterErrors;
    bool fragmentBoundaryInserted = false;

    for (std::size_t index = 0; index < document.operations.size(); ++index) {
        const Operation& operation = document.operations[index];
        const int line = operation.sourceLine > 0
                             ? static_cast<int>(operation.sourceLine)
                             : static_cast<int>(index + 1);
        vn::Command command;
        command.line = line;
        command.sourceId = operation.sourceId;
        command.operationId = operation.operationId;

        if (operation.kind == "scene") {
            command.type = "chapter";
            Add(command, "title", Argument(operation, "title"));
        } else if (operation.kind == "fragment") {
            if (!fragmentBoundaryInserted) {
                vn::Command skipFragments;
                skipFragments.type = "jump";
                skipFragments.line = line;
                skipFragments.sourceId = operation.sourceId;
                skipFragments.operationId = operation.operationId;
                Add(skipFragments, "target", "@document-end");
                commands.push_back(std::move(skipFragments));
                fragmentBoundaryInserted = true;
            }
            commands.push_back(Label(Argument(operation, "target"), line,
                                     operation.sourceId, operation.operationId));
            continue;
        } else if (operation.kind == "callFragment") {
            command.type = "call";
            Add(command, "target", LabelTarget(Argument(operation, "target")));
        } else if (operation.kind == "return") {
            command.type = "return";
        } else if (operation.kind == "endStory") {
            command.type = "jump";
            Add(command, "target", "@document-end");
        } else if (operation.kind == "dialogue" || operation.kind == "narration") {
            command.type = "say";
            Add(command, "speaker", Argument(operation, "speaker"));
            Add(command, "char", Argument(operation, "character"));
            Add(command, "value", Argument(operation, "text"));
            Add(command, "color", Argument(operation, "color"));
            Add(command, "outline", Argument(operation, "outline"));
            Add(command, "speed", Argument(operation, "speed"));
            Add(command, "effect", Argument(operation, "effect"));
            Add(command, "voice", Argument(operation, "voice"));
            Add(command, "textId", operation.sourceId);
        } else if (operation.kind == "choiceOption") {
            command.type = "choice";
            Add(command, "text", Argument(operation, "text"));
            Add(command, "textId", operation.sourceId);
            Add(command, "target", LabelTarget(Argument(operation, "target")));
        } else if (operation.kind == "background") {
            command.type = "bg";
            Add(command, "file", Argument(operation, "asset"));
            Add(command, "rule", Argument(operation, "rule"));
            Add(command, "time", DurationMilliseconds(Argument(operation, "duration")));
            Add(command, "vague", Argument(operation, "vague"));
            Add(command, "transition", Argument(operation, "transition"));
            for (const char* name : {"x", "y", "zoom", "rotate", "anchor", "ease"})
                Add(command, name, Argument(operation, name));
            Add(command, "from_x", Argument(operation, "fromX"));
            Add(command, "from_y", Argument(operation, "fromY"));
            Add(command, "from_zoom", Argument(operation, "fromZoom"));
        } else if (operation.kind == "image") {
            command.type = "layer";
            Add(command, "name", Argument(operation, "name"));
            Add(command, "file", Argument(operation, "asset"));
            Add(command, "time", DurationMilliseconds(Argument(operation, "duration")));
            for (const char* name : {"x", "y", "zoom", "rotate", "anchor", "alpha", "z", "ease"})
                Add(command, name, Argument(operation, name));
            Add(command, "from_x", Argument(operation, "fromX"));
            Add(command, "from_y", Argument(operation, "fromY"));
            Add(command, "from_zoom", Argument(operation, "fromZoom"));
            Add(command, "from_alpha", Argument(operation, "fromAlpha"));
        } else if (operation.kind == "hideImage") {
            command.type = "layer_clear";
            Add(command, "name", Argument(operation, "name"));
            Add(command, "time", DurationMilliseconds(Argument(operation, "duration")));
        } else if (operation.kind == "transition") {
            command.type = "screen_transition";
            Add(command, "time", DurationMilliseconds(Argument(operation, "duration")));
            Add(command, "rule", Argument(operation, "rule"));
            Add(command, "vague", Argument(operation, "vague"));
            Add(command, "wait", Argument(operation, "wait"));
        } else if (operation.kind == "showCharacter") {
            command.type = "char";
            Add(command, "id", Argument(operation, "character"));
            Add(command, "file", Argument(operation, "sprite"));
            Add(command, "expression", Argument(operation, "expression"));
            Add(command, "pos", Argument(operation, "position"));
            Add(command, "x", Argument(operation, "x"));
            Add(command, "y", Argument(operation, "y"));
            Add(command, "scale", Argument(operation, "scale"));
            const auto transition = Argument(operation, "transition");
            if (transition == "auto") Add(command, "transitionMode", transition);
            else Add(command, "transition", transition);
        } else if (operation.kind == "hideCharacter") {
            command.type = "char_clear";
            Add(command, "id", Argument(operation, "character"));
            Add(command, "transition", Argument(operation, "transition"));
        } else if (operation.kind == "hideCharacters") {
            command.type = "char_clear_all";
            Add(command, "transition", Argument(operation, "transition"));
        } else if (operation.kind == "voice" || operation.kind == "bgm" ||
                   operation.kind == "soundEffect" || operation.kind == "ambience") {
            command.type = operation.kind == "soundEffect" ? "se" : operation.kind;
            Add(command, "file", Argument(operation, "asset"));
            Add(command, "fade", DurationMilliseconds(Argument(operation, "fade")));
            if (operation.kind == "bgm") Add(command, "intro", Argument(operation, "intro"));
            if (operation.kind == "bgm" || operation.kind == "ambience")
                Add(command, "loop", Argument(operation, "loop"));
        } else if (operation.kind == "stopBgm" || operation.kind == "stopAmbience") {
            command.type = operation.kind == "stopBgm" ? "stopbgm" : "stopambience";
            Add(command, "fade", DurationMilliseconds(Argument(operation, "fade")));
        } else if (operation.kind == "video") {
            command.type = "video";
            Add(command, "file", Argument(operation, "asset"));
            Add(command, "skippable", Argument(operation, "skippable"));
        } else if (operation.kind == "setVariable") {
            command.type = "var";
            Add(command, "name", Argument(operation, "name"));
            const std::string& scope = Argument(operation, "scope");
            if (!scope.empty()) {
                if (scope != "session" && scope != "profile") {
                    adapterErrors.push_back(
                        "invalid variable scope at Runtime IR operation " +
                        std::to_string(index));
                    continue;
                }
                command.typedArgs["scope"] = Variant(scope);
            }
            const auto add = operation.arguments.find("add");
            const auto value = operation.arguments.find("value");
            const std::string& type = Argument(operation, "type");
            if (add != operation.arguments.end()) {
                command.typedArgs["add"] = ParseLiteral(add->second);
            } else if (value != operation.arguments.end()) {
                if (type.empty()) {
                    command.typedArgs["value"] = ParseLiteral(value->second);
                } else if (auto converted = ParseCatalogLiteral(value->second, type)) {
                    command.typedArgs["value"] = std::move(*converted);
                } else {
                    adapterErrors.push_back(
                        "invalid " + type +
                        " variable value at Runtime IR operation " +
                        std::to_string(index));
                    continue;
                }
            }
        } else if (operation.kind == "condition") {
            const auto expression = ParseRuntimeExpression(Argument(operation, "expression"));
            const auto found = conditions.find(index);
            if (!expression || found == conditions.end()) {
                adapterErrors.push_back("invalid condition at Runtime IR operation " +
                                        std::to_string(index));
                continue;
            }
            const std::string suffix = operation.sourceId;
            command.type = "branch";
            command.typedArgs["expression"] = vn::ExpressionToValue(*expression);
            Add(command, "trueTarget", "@if-true-" + suffix);
            Add(command, "falseTarget",
                found->second.elseIndex == (std::numeric_limits<std::size_t>::max)()
                    ? "@if-end-" + suffix
                    : "@if-else-" + suffix);
            commands.push_back(std::move(command));
            commands.push_back(Label("if-true-" + suffix, line, operation.sourceId,
                                     operation.operationId));
            continue;
        } else if (operation.kind == "else") {
            const auto owner = ownerByBoundary.find(index);
            if (owner == ownerByBoundary.end()) continue;
            const std::string suffix = document.operations[owner->second].sourceId;
            command.type = "jump";
            Add(command, "target", "@if-end-" + suffix);
            commands.push_back(std::move(command));
            commands.push_back(Label("if-else-" + suffix, line, operation.sourceId,
                                     operation.operationId));
            continue;
        } else if (operation.kind == "endCondition") {
            const auto owner = ownerByBoundary.find(index);
            if (owner != ownerByBoundary.end()) {
                commands.push_back(
                    Label("if-end-" + document.operations[owner->second].sourceId, line,
                          operation.sourceId, operation.operationId));
            }
            continue;
        } else if (operation.kind == "label") {
            command.type = "label";
            Add(command, "name", LabelTarget(Argument(operation, "target")));
            // Runtime IR uses stable hashed targets for control flow, while
            // replay/flowchart UIs are naturally authored against the public
            // Story label. Preserve that source spelling as a second lookup
            // key instead of forcing games to reproduce the compiler hash.
            const std::string authored = Trim(operation.text);
            if (authored.size() > 1 && authored.front() == '*') {
                const std::string alias = Trim(std::string_view(authored).substr(1));
                if (!alias.empty() && alias != Argument(operation, "target"))
                    Add(command, "alias", alias);
            }
        } else if (operation.kind == "jump") {
            command.type = "jump";
            Add(command, "target", LabelTarget(Argument(operation, "target")));
        } else if (operation.kind == "wait") {
            command.type = "wait";
            Add(command, "ms", DurationMilliseconds(Argument(operation, "duration")));
        } else if (operation.kind == "timeline") {
            if (Argument(operation, "mode") == "stage-motion") {
                command.type = "stage_motion";
                Add(command, "path", Argument(operation, "timeline"));
                Add(command, "await", Argument(operation, "await"));
                Add(command, "speed", Argument(operation, "speed"));
            } else if (Argument(operation, "mode") == "stage-motion-stop") {
                command.type = "stage_motion_stop";
            } else if (Argument(operation, "mode") == "move") {
                command.type = "char_move";
                Add(command, "id", Argument(operation, "target"));
                Add(command, "pos", Argument(operation, "position"));
                for (const char* name : {"x", "y", "scale", "ease", "wait"}) {
                    Add(command, name, Argument(operation, name));
                }
                Add(command, "duration", DurationMilliseconds(Argument(operation, "duration")));
            } else if (Argument(operation, "mode") == "animate") {
                command.type = "anim";
                for (const char* name : {"target", "x", "y", "scale", "alpha", "ease", "wait"}) {
                    Add(command, name, Argument(operation, name));
                }
                Add(command, "duration", DurationMilliseconds(Argument(operation, "duration")));
            } else {
                command.type = "animation";
                Add(command, "clip", Argument(operation, "timeline"));
                Add(command, "await", Argument(operation, "await"));
                Add(command, "speed", Argument(operation, "speed"));
            }
        } else if (operation.kind == "effect") {
            command.type = "screen_effect";
            Add(command, "preset", Argument(operation, "value"));
        } else if (operation.kind == "shake") {
            command.type = "shake";
            Add(command, "ms", DurationMilliseconds(Argument(operation, "duration")));
            Add(command, "amp", Argument(operation, "amplitude"));
            Add(command, "target", Argument(operation, "target"));
        } else if (operation.kind == "actorAction") {
            command.type = "actor_action";
            Add(command, "id", Argument(operation, "character"));
            Add(command, "preset", Argument(operation, "preset"));
            Add(command, "size", Argument(operation, "size"));
            Add(command, "time", DurationMilliseconds(Argument(operation, "time")));
            Add(command, "loop", Argument(operation, "loop"));
            Add(command, "ease", Argument(operation, "ease"));
        } else if (operation.kind == "stopSoundEffect") {
            command.type = "stopse";
            Add(command, "fade", DurationMilliseconds(Argument(operation, "fade")));
        } else if (operation.kind == "stopVoice") {
            command.type = "stopvoice";
        } else if (operation.kind == "ui") {
            command.type = "route";
            Add(command, "route", Argument(operation, "route"));
            Add(command, "operation", Argument(operation, "operation"));
        } else if (operation.kind == "hudVisibility") {
            command.type = "hud_visibility";
            Add(command, "visible", Argument(operation, "visible"));
        } else if (operation.kind == "unlock") {
            command.type = "unlock";
            Add(command, "kind", Argument(operation, "kind"));
            Add(command, "id", Argument(operation, "id"));
        } else if (operation.kind == "customNode") {
            command.type = Argument(operation, "type");
            const auto* descriptor = vn::CommandRegistry::Global().Find(command.type);
            if (descriptor) {
                (void)MapTypedCustomNode(command, *descriptor,
                                         Argument(operation, "value"), index,
                                         adapterErrors);
            } else {
                const std::string& payload = Argument(operation, "value");
                Add(command, "value", payload);
                // Actions are routed through ActionCatalog rather than the VN
                // CommandRegistry. Preserve their argument object as typed VM
                // data so RuntimeAssetResolver can recursively turn
                // asset:<uuid> tokens into ResourceRef values before either
                // PreviewHost or Player dispatches the Action.
                if (command.type == "action") {
                    const auto action =
                        nlohmann::json::parse(payload, nullptr, false);
                    if (!action.is_discarded() && action.is_object()) {
                        const auto arguments = action.find("arguments");
                        if (arguments != action.end() && arguments->is_object()) {
                            if (auto typed = UntypedJsonValue(*arguments)) {
                                command.typedArgs["arguments"] = std::move(*typed);
                            }
                        }
                    }
                }
            }
        } else if (operation.kind == "camera") {
            if (!Argument(operation, "value").empty()) {
                command.type = "screen_effect";
                Add(command, "preset", Argument(operation, "value"));
            } else {
                command.type = "camera";
                for (const char* name : {"x", "y", "zoom", "ease", "wait"})
                    Add(command, name, Argument(operation, name));
                Add(command, "duration", DurationMilliseconds(Argument(operation, "duration")));
            }
        } else {
            adapterErrors.push_back(
                "unknown or structural Runtime operation kind '" +
                operation.kind + "' at Runtime IR operation " +
                std::to_string(index));
            continue;
        }
        commands.push_back(std::move(command));
    }

    if (fragmentBoundaryInserted ||
        std::ranges::any_of(document.operations,
                            [](const Operation& operation) { return operation.kind == "endStory"; })) {
        commands.push_back(Label("document-end", static_cast<int>(document.operations.size() + 1)));
    }

    vn::Program program = vn::CompileProgram(std::move(commands));
    program.documentId = document.documentId;
    program.errors.insert(program.errors.end(), adapterErrors.begin(), adapterErrors.end());
    return program;
}

}  // namespace px
