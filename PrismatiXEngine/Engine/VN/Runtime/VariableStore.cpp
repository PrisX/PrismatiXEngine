#include "Engine/VN/Runtime/VariableStore.h"

#include <limits>
#include <sstream>
#include <string>

namespace px::vn {

void VariableStore::SetValue(const std::string& name, Value value, const VariableScope scope) {
    if (name.empty()) return;
    m_typedValues[name] = {value.Clone(), scope};
    if (scope == VariableScope::Profile) m_profileKeys.insert(name);
    else m_profileKeys.erase(name);
    if (const auto* integer = value.TryGet<std::int64_t>();
        integer && *integer >= (std::numeric_limits<int>::min)() &&
        *integer <= (std::numeric_limits<int>::max)()) {
        m_values[name] = static_cast<int>(*integer);
    } else {
        m_values.erase(name);
    }
    if (scope == VariableScope::Profile && m_profileWrite)
        m_profileWrite(name, m_typedValues.at(name).value);
}

const Value* VariableStore::GetValue(const std::string_view name) const {
    const auto found = m_typedValues.find(std::string(name));
    return found == m_typedValues.end() ? nullptr : &found->second.value;
}

Result<Value> VariableStore::Evaluate(const Expression& expression) const {
    return EvaluateExpression(expression, [this](const std::string_view name) -> std::optional<Value> {
        const Value* value = GetValue(name);
        return value ? std::optional<Value>(value->Clone()) : std::nullopt;
    });
}

void VariableStore::Set(const std::string& name, const int value,
                        const VariableScope scope) {
    SetValue(name, Value(value), scope);
}

void VariableStore::Add(const std::string& name, const int delta,
                        const VariableScope scope) {
    Set(name, Get(name) + delta,
        m_profileKeys.contains(name) ? VariableScope::Profile : scope);
}

int VariableStore::Get(const std::string& name) const {
    const Value* value = GetValue(name);
    if (!value) return 0;
    if (const auto* integer = value->TryGet<std::int64_t>()) return static_cast<int>(*integer);
    if (const auto* boolean = value->TryGet<bool>()) return *boolean ? 1 : 0;
    if (const auto* number = value->TryGet<double>()) return static_cast<int>(*number);
    return 0;
}

bool VariableStore::Has(const std::string& name) const {
    return m_typedValues.contains(name);
}

bool VariableStore::Evaluate(const std::string& name, const std::string& op, int rhs) const {
    const int lhs = Get(name);
    if (op.empty()) {
        return lhs != 0;
    }
    if (op == "==") return lhs == rhs;
    if (op == "!=") return lhs != rhs;
    if (op == ">") return lhs > rhs;
    if (op == "<") return lhs < rhs;
    if (op == ">=") return lhs >= rhs;
    if (op == "<=") return lhs <= rhs;
    return false;
}

std::string VariableStore::Substitute(std::string_view text) const {
    const auto isIdentifier = [](std::string_view key) {
        if (key.empty()) return false;
        for (char c : key) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_';
            if (!ok) return false;
        }
        return true;
    };

    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '{') {
            const std::size_t close = text.find('}', i);
            if (close != std::string_view::npos) {
                const std::string key(text.substr(i + 1, close - i - 1));
                // Only substitute identifier-like keys; anything else (inline
                // tags like {w=300}, literal braces) passes through untouched.
                if (isIdentifier(key)) {
                    if (const Value* value = GetValue(key)) {
                        if (const auto* integer = value->TryGet<std::int64_t>()) {
                            out += std::to_string(*integer);
                        } else if (const auto* number = value->TryGet<double>()) {
                            std::ostringstream stream;
                            stream << *number;
                            out += stream.str();
                        } else if (const auto* boolean = value->TryGet<bool>()) {
                            out += *boolean ? "true" : "false";
                        } else if (const auto* string = value->TryGet<std::string>()) {
                            out += *string;
                        }
                    }
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

void VariableStore::Reset(bool keepPersistent) {
    if (!keepPersistent) {
        m_values.clear();
        m_typedValues.clear();
        m_profileKeys.clear();
        return;
    }
    std::unordered_map<std::string, int> kept;
    std::unordered_map<std::string, VariableEntry> typedKept;
    for (const std::string& key : m_profileKeys) {
        if (auto it = m_values.find(key); it != m_values.end()) {
            kept[key] = it->second;
        }
        if (auto it = m_typedValues.find(key); it != m_typedValues.end()) {
            typedKept.emplace(key, VariableEntry{it->second.value.Clone(), VariableScope::Profile});
        }
    }
    m_values = std::move(kept);
    m_typedValues = std::move(typedKept);
}

}
