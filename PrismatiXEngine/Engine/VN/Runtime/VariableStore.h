#pragma once

#include "Engine/VN/Expression/Expression.h"

#include <optional>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace px::vn {

enum class VariableScope : std::uint8_t { Profile, Session, Temporary };

struct VariableEntry {
    Value value;
    VariableScope scope = VariableScope::Session;
};

class VariableStore {
public:
    using ProfileWriteHandler =
        std::function<void(std::string_view name, const Value& value)>;

    void SetProfileWriteHandler(ProfileWriteHandler handler) {
        m_profileWrite = std::move(handler);
    }
    void SetValue(const std::string& name, Value value,
                  VariableScope scope = VariableScope::Session);
    [[nodiscard]] const Value* GetValue(std::string_view name) const;
    [[nodiscard]] Result<Value> Evaluate(const Expression& expression) const;

    void Set(const std::string& name, int value,
             VariableScope scope = VariableScope::Session);
    void Add(const std::string& name, int delta,
             VariableScope scope = VariableScope::Session);
    [[nodiscard]] int Get(const std::string& name) const;
    [[nodiscard]] bool Has(const std::string& name) const;

    [[nodiscard]] bool Evaluate(const std::string& name, const std::string& op, int rhs) const;

    [[nodiscard]] std::string Substitute(std::string_view text) const;

    void Reset(bool keepPersistent);

    [[nodiscard]] const std::unordered_map<std::string, int>& All() const { return m_values; }
    [[nodiscard]] const std::unordered_map<std::string, VariableEntry>& Values() const {
        return m_typedValues;
    }
    [[nodiscard]] const std::set<std::string>& ProfileKeys() const {
        return m_profileKeys;
    }

private:
    std::unordered_map<std::string, int> m_values;
    std::unordered_map<std::string, VariableEntry> m_typedValues;
    std::set<std::string> m_profileKeys;
    ProfileWriteHandler m_profileWrite;
};

}
