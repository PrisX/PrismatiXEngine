#include "Engine/Session/RuntimeIrAdapter.h"

#include "Engine/VN/Expression/Expression.h"

#include <charconv>
#include <limits>
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
    const auto numberResult =
        std::from_chars(value.data(), value.data() + value.size(), number);
    if (numberResult.ec == std::errc{} && numberResult.ptr == value.data() + value.size()) {
        return Variant(number);
    }
    return Variant(value);
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
    const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != number.data() + number.size() || parsed < 0.0) {
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

vn::Command Label(std::string name, const int line) {
    vn::Command command;
    command.type = "label";
    command.line = line;
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
        const int line = static_cast<int>(index + 1);
        vn::Command command;
        command.line = line;

        if (operation.kind == "scene") {
            command.type = "chapter";
            Add(command, "title", Argument(operation, "title"));
        } else if (operation.kind == "fragment") {
            if (!fragmentBoundaryInserted) {
                vn::Command skipFragments;
                skipFragments.type = "jump";
                skipFragments.line = line;
                Add(skipFragments, "target", "@document-end");
                commands.push_back(std::move(skipFragments));
                fragmentBoundaryInserted = true;
            }
            commands.push_back(Label(Argument(operation, "target"), line));
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
            Add(command, "value", Argument(operation, "text"));
            Add(command, "textId", operation.sourceId);
        } else if (operation.kind == "choiceOption") {
            command.type = "choice";
            Add(command, "text", Argument(operation, "text"));
            Add(command, "textId", operation.sourceId);
            Add(command, "target", LabelTarget(Argument(operation, "target")));
        } else if (operation.kind == "background") {
            command.type = "bg";
            Add(command, "file", Argument(operation, "asset"));
        } else if (operation.kind == "showCharacter") {
            command.type = "char";
            Add(command, "id", Argument(operation, "character"));
        } else if (operation.kind == "hideCharacter") {
            command.type = "char_clear";
            Add(command, "id", Argument(operation, "character"));
        } else if (operation.kind == "voice" || operation.kind == "bgm" ||
                   operation.kind == "soundEffect") {
            command.type = operation.kind == "soundEffect" ? "se" : operation.kind;
            Add(command, "file", Argument(operation, "asset"));
        } else if (operation.kind == "setVariable") {
            command.type = "var";
            Add(command, "name", Argument(operation, "name"));
            if (!Argument(operation, "value").empty()) {
                command.typedArgs["value"] = ParseLiteral(Argument(operation, "value"));
            }
        } else if (operation.kind == "condition") {
            const auto expression = ParseSimpleExpression(Argument(operation, "expression"));
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
            commands.push_back(Label("if-true-" + suffix, line));
            continue;
        } else if (operation.kind == "else") {
            const auto owner = ownerByBoundary.find(index);
            if (owner == ownerByBoundary.end()) continue;
            const std::string suffix = document.operations[owner->second].sourceId;
            command.type = "jump";
            Add(command, "target", "@if-end-" + suffix);
            commands.push_back(std::move(command));
            commands.push_back(Label("if-else-" + suffix, line));
            continue;
        } else if (operation.kind == "endCondition") {
            const auto owner = ownerByBoundary.find(index);
            if (owner != ownerByBoundary.end()) {
                commands.push_back(
                    Label("if-end-" + document.operations[owner->second].sourceId, line));
            }
            continue;
        } else if (operation.kind == "label") {
            command.type = "label";
            Add(command, "name", LabelTarget(Argument(operation, "target")));
        } else if (operation.kind == "jump") {
            command.type = "jump";
            Add(command, "target", LabelTarget(Argument(operation, "target")));
        } else if (operation.kind == "wait") {
            command.type = "wait";
            Add(command, "ms", DurationMilliseconds(Argument(operation, "duration")));
        } else if (operation.kind == "timeline") {
            command.type = "animation";
            Add(command, "clip", Argument(operation, "timeline"));
        } else if (operation.kind == "effect") {
            command.type = "screen_effect";
            Add(command, "preset", Argument(operation, "value"));
        } else if (operation.kind == "ui") {
            command.type = "route";
            Add(command, "route", Argument(operation, "value"));
        } else if (operation.kind == "lua") {
            command.type = "lua";
            Add(command, "fn", Argument(operation, "value"));
        } else if (operation.kind == "camera") {
            command.type = "camera";
            Add(command, "preset", Argument(operation, "value"));
        } else {
            continue;
        }
        commands.push_back(std::move(command));
    }

    if (fragmentBoundaryInserted) {
        commands.push_back(Label("document-end", static_cast<int>(document.operations.size() + 1)));
    }

    vn::Program program = vn::CompileProgram(std::move(commands));
    program.errors.insert(program.errors.end(), adapterErrors.begin(), adapterErrors.end());
    return program;
}

}  // namespace px
