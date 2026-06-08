#pragma once

#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace px::vn {

class VariableStore {
public:
    void Set(const std::string& name, int value, bool persistent = false);
    void Add(const std::string& name, int delta, bool persistent = false);
    [[nodiscard]] int Get(const std::string& name) const;
    [[nodiscard]] bool Has(const std::string& name) const;

    [[nodiscard]] bool Evaluate(const std::string& name, const std::string& op, int rhs) const;

    [[nodiscard]] std::string Substitute(std::string_view text) const;

    void Reset(bool keepPersistent);

    [[nodiscard]] const std::unordered_map<std::string, int>& All() const { return m_values; }
    [[nodiscard]] const std::set<std::string>& PersistentKeys() const { return m_persistent; }

private:
    std::unordered_map<std::string, int> m_values;
    std::set<std::string> m_persistent;
};

}
