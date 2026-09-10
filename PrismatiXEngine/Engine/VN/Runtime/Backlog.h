#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
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
    // Runtime-assigned append identity. Unlike a physical vector index this
    // remains stable when the bounded backlog evicts older entries.
    std::uint64_t sequence = 0;
};

class Backlog {
public:
    std::uint64_t Push(
        const std::string& speaker, const std::string& text,
        const std::string& voice = "", bool isChoice = false,
        const std::string& sourceId = {},
        const std::string& operationId = {}, std::uint64_t sequence = 0);
    void Clear() { m_entries.clear(); }
    // Drops entries past `size` (rollback rewinds the log in place).
    void Truncate(std::size_t size) {
        if (size < m_entries.size()) {
            m_entries.resize(size);
        }
    }

    [[nodiscard]] const std::vector<BacklogEntry>& Entries() const { return m_entries; }
    [[nodiscard]] std::size_t Size() const { return m_entries.size(); }
    [[nodiscard]] std::uint64_t LastSequence() const { return m_lastSequence; }
    [[nodiscard]] const BacklogEntry* Find(std::uint64_t sequence) const;
    [[nodiscard]] std::span<const BacklogEntry> EntriesAfter(
        std::uint64_t sequence) const;

    void SetCapacity(std::size_t capacity) { m_capacity = capacity; }

private:
    std::vector<BacklogEntry> m_entries;
    std::size_t m_capacity = 500;
    std::uint64_t m_lastSequence = 0;
};

}
