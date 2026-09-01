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
    // Stable content identity allows runtime locale switching and save
    // migration to re-resolve backlog text without guessing from line text.
    std::string sourceId;
    std::string operationId;
};

class Backlog {
public:
    void Push(const std::string& speaker, const std::string& text, const std::string& voice = "",
              bool isChoice = false, const std::string& sourceId = {},
              const std::string& operationId = {});
    void Clear() { m_entries.clear(); }
    // Drops entries past `size` (rollback rewinds the log in place).
    void Truncate(std::size_t size) {
        if (size < m_entries.size()) {
            m_entries.resize(size);
        }
    }

    [[nodiscard]] const std::vector<BacklogEntry>& Entries() const { return m_entries; }
    [[nodiscard]] std::size_t Size() const { return m_entries.size(); }

    void SetCapacity(std::size_t capacity) { m_capacity = capacity; }

private:
    std::vector<BacklogEntry> m_entries;
    std::size_t m_capacity = 500;
};

}
