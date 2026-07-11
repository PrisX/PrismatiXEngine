#include "Engine/VN/Expression/Expression.h"

#include <cmath>
#include <limits>

namespace px::vn {
namespace {

diag::Diagnostic ExpressionError(std::string code, std::string message,
                                 const std::string& path = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "VN.Expression",
                                .message = std::move(message)};
    diagnostic.source.path = path;
    return diagnostic;
}

std::optional<double> NumberValue(const Value& value) {
    if (const auto* integer = value.TryGet<std::int64_t>()) {
        return static_cast<double>(*integer);
    }
    if (const auto* number = value.TryGet<double>()) return *number;
    return std::nullopt;
}

bool IsInteger(const Value& value) {
    return value.Type() == VariantType::Integer;
}

Result<Value> TypeError(const ExpressionOperator op, const char* expected) {
    return Result<Value>::Failure(ExpressionError(
        "PXEXPR7201", std::string("Operator '") + ToString(op) + "' requires " + expected));
}

std::optional<ExpressionOperator> OperatorFromString(const std::string& value) {
    for (const auto op : {ExpressionOperator::Not, ExpressionOperator::Negate,
                          ExpressionOperator::Add, ExpressionOperator::Subtract,
                          ExpressionOperator::Multiply, ExpressionOperator::Divide,
                          ExpressionOperator::Modulo, ExpressionOperator::Equal,
                          ExpressionOperator::NotEqual, ExpressionOperator::Less,
                          ExpressionOperator::LessEqual, ExpressionOperator::Greater,
                          ExpressionOperator::GreaterEqual, ExpressionOperator::And,
                          ExpressionOperator::Or}) {
        if (value == ToString(op)) return op;
    }
    return std::nullopt;
}

Result<Value> EvaluateBinary(const ExpressionOperator op, const Value& lhs, const Value& rhs) {
    if (op == ExpressionOperator::Equal || op == ExpressionOperator::NotEqual) {
        bool equal = false;
        const auto leftNumber = NumberValue(lhs);
        const auto rightNumber = NumberValue(rhs);
        if (leftNumber && rightNumber) {
            equal = *leftNumber == *rightNumber;
        } else {
            equal = lhs == rhs;
        }
        return Result<Value>::Success(Value(op == ExpressionOperator::Equal ? equal : !equal));
    }

    if (op == ExpressionOperator::Add && lhs.Type() == VariantType::String &&
        rhs.Type() == VariantType::String) {
        return Result<Value>::Success(
            Value(*lhs.TryGet<std::string>() + *rhs.TryGet<std::string>()));
    }

    if (op == ExpressionOperator::Less || op == ExpressionOperator::LessEqual ||
        op == ExpressionOperator::Greater || op == ExpressionOperator::GreaterEqual) {
        if (lhs.Type() == VariantType::String && rhs.Type() == VariantType::String) {
            const auto& left = *lhs.TryGet<std::string>();
            const auto& right = *rhs.TryGet<std::string>();
            bool result = false;
            if (op == ExpressionOperator::Less) result = left < right;
            if (op == ExpressionOperator::LessEqual) result = left <= right;
            if (op == ExpressionOperator::Greater) result = left > right;
            if (op == ExpressionOperator::GreaterEqual) result = left >= right;
            return Result<Value>::Success(Value(result));
        }
    }

    const auto left = NumberValue(lhs);
    const auto right = NumberValue(rhs);
    if (!left || !right) return TypeError(op, "numeric operands");

    switch (op) {
        case ExpressionOperator::Add:
            if (IsInteger(lhs) && IsInteger(rhs)) {
                return Result<Value>::Success(Value(*lhs.TryGet<std::int64_t>() +
                                                    *rhs.TryGet<std::int64_t>()));
            }
            return Result<Value>::Success(Value(*left + *right));
        case ExpressionOperator::Subtract:
            if (IsInteger(lhs) && IsInteger(rhs)) {
                return Result<Value>::Success(Value(*lhs.TryGet<std::int64_t>() -
                                                    *rhs.TryGet<std::int64_t>()));
            }
            return Result<Value>::Success(Value(*left - *right));
        case ExpressionOperator::Multiply:
            if (IsInteger(lhs) && IsInteger(rhs)) {
                return Result<Value>::Success(Value(*lhs.TryGet<std::int64_t>() *
                                                    *rhs.TryGet<std::int64_t>()));
            }
            return Result<Value>::Success(Value(*left * *right));
        case ExpressionOperator::Divide:
            if (*right == 0.0) {
                return Result<Value>::Failure(
                    ExpressionError("PXEXPR7202", "Division by zero"));
            }
            return Result<Value>::Success(Value(*left / *right));
        case ExpressionOperator::Modulo: {
            const auto* leftInteger = lhs.TryGet<std::int64_t>();
            const auto* rightInteger = rhs.TryGet<std::int64_t>();
            if (!leftInteger || !rightInteger) return TypeError(op, "integer operands");
            if (*rightInteger == 0) {
                return Result<Value>::Failure(
                    ExpressionError("PXEXPR7202", "Modulo by zero"));
            }
            return Result<Value>::Success(Value(*leftInteger % *rightInteger));
        }
        case ExpressionOperator::Less: return Result<Value>::Success(Value(*left < *right));
        case ExpressionOperator::LessEqual:
            return Result<Value>::Success(Value(*left <= *right));
        case ExpressionOperator::Greater: return Result<Value>::Success(Value(*left > *right));
        case ExpressionOperator::GreaterEqual:
            return Result<Value>::Success(Value(*left >= *right));
        default: return TypeError(op, "compatible operands");
    }
}

Result<Expression> ParseExpression(const Value& value, const std::string& path, const int depth) {
    if (depth > 64) {
        return Result<Expression>::Failure(
            ExpressionError("PXEXPR7210", "Expression nesting exceeds 64 levels", path));
    }
    const ValueMap* object = value.AsObject();
    if (!object) {
        return Result<Expression>::Failure(
            ExpressionError("PXEXPR7211", "Expression must be an object", path));
    }
    const auto kindIt = object->find("kind");
    if (kindIt == object->end() || !kindIt->second.TryGet<std::string>()) {
        return Result<Expression>::Failure(
            ExpressionError("PXEXPR7212", "Expression kind is missing", path));
    }
    const std::string& kind = *kindIt->second.TryGet<std::string>();
    if (kind == "literal") {
        const auto found = object->find("value");
        return found == object->end()
                   ? Result<Expression>::Failure(
                         ExpressionError("PXEXPR7213", "Literal value is missing", path))
                   : Result<Expression>::Success(Expression::Literal(found->second.Clone()));
    }
    if (kind == "variable") {
        const auto found = object->find("name");
        if (found == object->end() || !found->second.TryGet<std::string>() ||
            found->second.TryGet<std::string>()->empty()) {
            return Result<Expression>::Failure(
                ExpressionError("PXEXPR7214", "Variable name is missing", path));
        }
        return Result<Expression>::Success(Expression::Variable(*found->second.TryGet<std::string>()));
    }
    const auto opIt = object->find("op");
    if (opIt == object->end() || !opIt->second.TryGet<std::string>()) {
        return Result<Expression>::Failure(
            ExpressionError("PXEXPR7215", "Expression operator is missing", path));
    }
    const auto op = OperatorFromString(*opIt->second.TryGet<std::string>());
    if (!op) {
        return Result<Expression>::Failure(
            ExpressionError("PXEXPR7216", "Expression operator is unknown", path));
    }
    if (kind == "unary") {
        const auto operand = object->find("operand");
        if (operand == object->end()) {
            return Result<Expression>::Failure(
                ExpressionError("PXEXPR7217", "Unary operand is missing", path));
        }
        auto parsed = ParseExpression(operand->second, path, depth + 1);
        if (!parsed) return parsed;
        return Result<Expression>::Success(Expression::Unary(*op, parsed.TakeValue()));
    }
    if (kind == "binary") {
        const auto left = object->find("left");
        const auto right = object->find("right");
        if (left == object->end() || right == object->end()) {
            return Result<Expression>::Failure(
                ExpressionError("PXEXPR7218", "Binary operand is missing", path));
        }
        auto parsedLeft = ParseExpression(left->second, path, depth + 1);
        if (!parsedLeft) return parsedLeft;
        auto parsedRight = ParseExpression(right->second, path, depth + 1);
        if (!parsedRight) return parsedRight;
        return Result<Expression>::Success(
            Expression::Binary(*op, parsedLeft.TakeValue(), parsedRight.TakeValue()));
    }
    return Result<Expression>::Failure(
        ExpressionError("PXEXPR7219", "Expression kind is unknown", path));
}

}  // namespace

Expression Expression::Literal(Value value) {
    Expression result;
    result.literal = std::move(value);
    return result;
}

Expression Expression::Variable(std::string name) {
    Expression result;
    result.kind = ExpressionKind::Variable;
    result.variable = std::move(name);
    return result;
}

Expression Expression::Unary(const ExpressionOperator op, Expression operand) {
    Expression result;
    result.kind = ExpressionKind::Unary;
    result.op = op;
    result.left = std::make_shared<Expression>(std::move(operand));
    return result;
}

Expression Expression::Binary(const ExpressionOperator op, Expression lhs, Expression rhs) {
    Expression result;
    result.kind = ExpressionKind::Binary;
    result.op = op;
    result.left = std::make_shared<Expression>(std::move(lhs));
    result.right = std::make_shared<Expression>(std::move(rhs));
    return result;
}

Result<Value> EvaluateExpression(const Expression& expression, const VariableResolver& variables) {
    if (expression.kind == ExpressionKind::Literal) {
        return Result<Value>::Success(expression.literal.Clone());
    }
    if (expression.kind == ExpressionKind::Variable) {
        if (auto value = variables(expression.variable)) {
            return Result<Value>::Success(value->Clone());
        }
        return Result<Value>::Failure(ExpressionError(
            "PXEXPR7203", "Unknown variable '" + expression.variable + "'"));
    }
    if (!expression.left) {
        return Result<Value>::Failure(
            ExpressionError("PXEXPR7204", "Expression is missing its left operand"));
    }
    auto left = EvaluateExpression(*expression.left, variables);
    if (!left) return left;
    if (expression.kind == ExpressionKind::Unary) {
        if (expression.op == ExpressionOperator::Not) {
            const auto* value = left.Value().TryGet<bool>();
            return value ? Result<Value>::Success(Value(!*value))
                         : TypeError(expression.op, "a boolean operand");
        }
        if (expression.op == ExpressionOperator::Negate) {
            if (const auto* value = left.Value().TryGet<std::int64_t>()) {
                if (*value == (std::numeric_limits<std::int64_t>::min)()) {
                    return Result<Value>::Failure(
                        ExpressionError("PXEXPR7205", "Integer negation overflow"));
                }
                return Result<Value>::Success(Value(-*value));
            }
            if (const auto* value = left.Value().TryGet<double>()) {
                return Result<Value>::Success(Value(-*value));
            }
        }
        return TypeError(expression.op, "a numeric operand");
    }

    if (expression.op == ExpressionOperator::And || expression.op == ExpressionOperator::Or) {
        const auto* leftBoolean = left.Value().TryGet<bool>();
        if (!leftBoolean) return TypeError(expression.op, "boolean operands");
        if (expression.op == ExpressionOperator::And && !*leftBoolean) {
            return Result<Value>::Success(Value(false));
        }
        if (expression.op == ExpressionOperator::Or && *leftBoolean) {
            return Result<Value>::Success(Value(true));
        }
    }
    if (!expression.right) {
        return Result<Value>::Failure(
            ExpressionError("PXEXPR7206", "Expression is missing its right operand"));
    }
    auto right = EvaluateExpression(*expression.right, variables);
    if (!right) return right;
    if (expression.op == ExpressionOperator::And || expression.op == ExpressionOperator::Or) {
        const auto* rightBoolean = right.Value().TryGet<bool>();
        return rightBoolean ? Result<Value>::Success(Value(*rightBoolean))
                            : TypeError(expression.op, "boolean operands");
    }
    return EvaluateBinary(expression.op, left.Value(), right.Value());
}

Value ExpressionToValue(const Expression& expression) {
    ValueMap object;
    if (expression.kind == ExpressionKind::Literal) {
        object["kind"] = "literal";
        object["value"] = expression.literal.Clone();
    } else if (expression.kind == ExpressionKind::Variable) {
        object["kind"] = "variable";
        object["name"] = expression.variable;
    } else if (expression.kind == ExpressionKind::Unary) {
        object["kind"] = "unary";
        object["op"] = ToString(expression.op);
        object["operand"] = expression.left ? ExpressionToValue(*expression.left) : Value{};
    } else {
        object["kind"] = "binary";
        object["op"] = ToString(expression.op);
        object["left"] = expression.left ? ExpressionToValue(*expression.left) : Value{};
        object["right"] = expression.right ? ExpressionToValue(*expression.right) : Value{};
    }
    return Value(std::move(object));
}

Result<Expression> ExpressionFromValue(const Value& value, const std::string& sourcePath) {
    return ParseExpression(value, sourcePath, 0);
}

const char* ToString(const ExpressionOperator op) {
    switch (op) {
        case ExpressionOperator::Not: return "not";
        case ExpressionOperator::Negate: return "negate";
        case ExpressionOperator::Add: return "add";
        case ExpressionOperator::Subtract: return "subtract";
        case ExpressionOperator::Multiply: return "multiply";
        case ExpressionOperator::Divide: return "divide";
        case ExpressionOperator::Modulo: return "modulo";
        case ExpressionOperator::Equal: return "equal";
        case ExpressionOperator::NotEqual: return "not-equal";
        case ExpressionOperator::Less: return "less";
        case ExpressionOperator::LessEqual: return "less-equal";
        case ExpressionOperator::Greater: return "greater";
        case ExpressionOperator::GreaterEqual: return "greater-equal";
        case ExpressionOperator::And: return "and";
        case ExpressionOperator::Or: return "or";
    }
    return "unknown";
}

}  // namespace px::vn
