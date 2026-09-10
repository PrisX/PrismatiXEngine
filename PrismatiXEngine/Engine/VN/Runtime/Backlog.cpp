#include "Engine/VN/Runtime/Backlog.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace px::vn {

std::uint64_t Backlog::Push(
    const std::string& speaker, const std::string& text,
    const std::string& voice, const bool isChoice,
    const std::string& sourceId, const std::string& operationId,
    std::uint64_t sequence) {
    if (sequence == 0) {
        if (m_lastSequence == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("backlog sequence space exhausted");
        }
        sequence = ++m_lastSequence;
    } else {
        m_lastSequence = std::max(m_lastSequence, sequence);
    }
    m_entries.push_back(BacklogEntry{
        speaker, text, voice, isChoice, sourceId, operationId, sequence});
    if (m_entries.size() > m_capacity) {
        m_entries.erase(m_entries.begin(),
                        m_entries.begin() + (m_entries.size() - m_capacity));
    }
    return sequence;
}

const BacklogEntry* Backlog::Find(const std::uint64_t sequence) const {
    const auto found = std::find_if(
        m_entries.begin(), m_entries.end(),
        [sequence](const BacklogEntry& entry) {
            return entry.sequence == sequence;
        });
    return found == m_entries.end() ? nullptr : &*found;
}

std::span<const BacklogEntry> Backlog::EntriesAfter(
    const std::uint64_t sequence) const {
    const auto first = std::upper_bound(
        m_entries.begin(), m_entries.end(), sequence,
        [](const std::uint64_t value, const BacklogEntry& entry) {
            return value < entry.sequence;
        });
    return {first, m_entries.end()};
}

}
