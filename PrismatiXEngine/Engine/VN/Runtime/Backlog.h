#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace px::vn {

struct BacklogEntry {
    std::string speaker;
    std::string text;
    std::string voice;
    bool isChoice = false;
};

class Backlog {
public:
    void Push(const std::string& speaker, const std::string& text, const std::string& voice = "",
              bool isChoice = false);
    void Clear() { m_entries.clear(); }

    [[nodiscard]] const std::vector<BacklogEntry>& Entries() const { return m_entries; }
    [[nodiscard]] std::size_t Size() const { return m_entries.size(); }

    void SetCapacity(std::size_t capacity) { m_capacity = capacity; }

private:
    std::vector<BacklogEntry> m_entries;
    std::size_t m_capacity = 500;
};

}
