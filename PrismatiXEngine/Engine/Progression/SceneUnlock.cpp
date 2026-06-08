#include "Engine/Progression/SceneUnlock.h"

namespace px::progress {

bool SceneUnlock::IsUnlocked(const std::string& id) const {
    for (const Chapter& c : m_catalog) {
        if (c.id == id) {
            return c.alwaysUnlocked || m_profile.SceneUnlocked(id);
        }
    }
    return m_profile.SceneUnlocked(id);
}

}
