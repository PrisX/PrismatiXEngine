#include "Engine/Progression/CGGallery.h"

namespace px::progress {

int CGGallery::UnlockedCount() const {
    int n = 0;
    for (const Item& item : m_catalog) {
        if (m_profile.CGUnlocked(item.id)) {
            ++n;
        }
    }
    return n;
}

}
