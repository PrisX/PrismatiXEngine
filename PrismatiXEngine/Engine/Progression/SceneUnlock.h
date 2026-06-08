#pragma once

#include <string>
#include <vector>

#include "Engine/Progression/GlobalProfile.h"

namespace px::progress {

// 劇情鎖
class SceneUnlock {
public:
    struct Chapter {
        std::string id;
        std::string title;
        std::string scriptPath;
        bool alwaysUnlocked = false;
    };

    explicit SceneUnlock(GlobalProfile& profile) : m_profile(profile) {}

    void SetCatalog(std::vector<Chapter> chapters) { m_catalog = std::move(chapters); }
    void AddToCatalog(Chapter chapter) { m_catalog.push_back(std::move(chapter)); }

    void Unlock(const std::string& id) { m_profile.UnlockScene(id); }
    [[nodiscard]] bool IsUnlocked(const std::string& id) const;

    [[nodiscard]] const std::vector<Chapter>& Catalog() const { return m_catalog; }

private:
    GlobalProfile& m_profile;
    std::vector<Chapter> m_catalog;
};

}  // namespace px::progress
