#pragma once

#include "Engine/Progression/GlobalProfile.h"

#include <string>
#include <vector>

namespace px::progress {

class CGGallery {
public:
    struct Item {
        std::string id;
        std::string title;
        std::string thumbnail;
        std::string image;
    };

    explicit CGGallery(GlobalProfile& profile) : m_profile(profile) {}

    void SetCatalog(std::vector<Item> items) { m_catalog = std::move(items); }
    void AddToCatalog(Item item) { m_catalog.push_back(std::move(item)); }

    void Unlock(const std::string& id) { m_profile.UnlockCG(id); }
    [[nodiscard]] bool IsUnlocked(const std::string& id) const { return m_profile.CGUnlocked(id); }

    [[nodiscard]] const std::vector<Item>& Catalog() const { return m_catalog; }
    [[nodiscard]] int UnlockedCount() const;

private:
    GlobalProfile& m_profile;
    std::vector<Item> m_catalog;
};

}
