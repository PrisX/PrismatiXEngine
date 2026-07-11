#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace px::vn {

// Value is the single runtime/scenario value model. Variant also carries
// editor-native values (resource refs, colors, vectors) while retaining the
// required null/bool/int/number/string/list/map data types.
using Value = Variant;
using ValueList = VariantArray;
using ValueMap = VariantObject;

enum class ExpressionKind : std::uint8_t { Literal, Variable, Unary, Binary };
enum class ExpressionOperator : std::uint8_t {
    Not,
    Negate,
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    And,
    Or,
};

struct Expression {
    ExpressionKind kind = ExpressionKind::Literal;
    ExpressionOperator op = ExpressionOperator::Equal;
    Value literal;
    std::string variable;
    std::shared_ptr<Expression> left;
    std::shared_ptr<Expression> right;

    [[nodiscard]] static Expression Literal(Value value);
    [[nodiscard]] static Expression Variable(std::string name);
    [[nodiscard]] static Expression Unary(ExpressionOperator op, Expression operand);
    [[nodiscard]] static Expression Binary(ExpressionOperator op, Expression lhs, Expression rhs);
};

using VariableResolver = std::function<std::optional<Value>(std::string_view)>;

[[nodiscard]] Result<Value> EvaluateExpression(const Expression& expression,
                                               const VariableResolver& variables);
[[nodiscard]] Value ExpressionToValue(const Expression& expression);
[[nodiscard]] Result<Expression> ExpressionFromValue(const Value& value,
                                                     const std::string& sourcePath = {});
[[nodiscard]] const char* ToString(ExpressionOperator op);

}  // namespace px::vn
