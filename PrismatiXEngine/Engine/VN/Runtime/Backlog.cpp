#include "Engine/VN/Runtime/Backlog.h"

namespace px::vn {

void Backlog::Push(const std::string& speaker, const std::string& text, const std::string& voice,
                   bool isChoice) {
    m_entries.push_back(BacklogEntry{ speaker, text, voice, isChoice });
    if (m_entries.size() > m_capacity) {
        m_entries.erase(m_entries.begin(),
                        m_entries.begin() + (m_entries.size() - m_capacity));
    }
}

}
