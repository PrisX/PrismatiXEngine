#pragma once

#include "Engine/VN/Expression/Expression.h"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace px::vn {

enum class VariableScope : std::uint8_t { Persistent, SaveLocal, Temporary };

struct VariableEntry {
    Value value;
    VariableScope scope = VariableScope::SaveLocal;
};

class VariableStore {
public:
    void SetValue(const std::string& name, Value value,
                  VariableScope scope = VariableScope::SaveLocal);
    [[nodiscard]] const Value* GetValue(std::string_view name) const;
    [[nodiscard]] Result<Value> Evaluate(const Expression& expression) const;

    void Set(const std::string& name, int value, bool persistent = false);
    void Add(const std::string& name, int delta, bool persistent = false);
    [[nodiscard]] int Get(const std::string& name) const;
    [[nodiscard]] bool Has(const std::string& name) const;

    [[nodiscard]] bool Evaluate(const std::string& name, const std::string& op, int rhs) const;

    [[nodiscard]] std::string Substitute(std::string_view text) const;

    void Reset(bool keepPersistent);

    [[nodiscard]] const std::unordered_map<std::string, int>& All() const { return m_values; }
    [[nodiscard]] const std::unordered_map<std::string, VariableEntry>& Values() const {
        return m_typedValues;
    }
    [[nodiscard]] const std::set<std::string>& PersistentKeys() const { return m_persistent; }

private:
    std::unordered_map<std::string, int> m_values;
    std::unordered_map<std::string, VariableEntry> m_typedValues;
    std::set<std::string> m_persistent;
};

}
