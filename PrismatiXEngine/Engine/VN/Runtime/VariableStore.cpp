#include "Engine/VN/Runtime/VariableStore.h"

#include <string>

namespace px::vn {

void VariableStore::Set(const std::string& name, int value, bool persistent) {
    m_values[name] = value;
    if (persistent) {
        m_persistent.insert(name);
    }
}

void VariableStore::Add(const std::string& name, int delta, bool persistent) {
    m_values[name] += delta;
    if (persistent) {
        m_persistent.insert(name);
    }
}

int VariableStore::Get(const std::string& name) const {
    auto it = m_values.find(name);
    return it != m_values.end() ? it->second : 0;
}

bool VariableStore::Has(const std::string& name) const {
    return m_values.find(name) != m_values.end();
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
                    out += std::to_string(Get(key));
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
        m_persistent.clear();
        return;
    }
    std::unordered_map<std::string, int> kept;
    for (const std::string& key : m_persistent) {
        if (auto it = m_values.find(key); it != m_values.end()) {
            kept[key] = it->second;
        }
    }
    m_values = std::move(kept);
}

}
